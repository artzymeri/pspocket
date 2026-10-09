#pragma once

// cellHttp's internals shared by its files (runtime/hle/src/cellHttp*.cpp, D-524): the error codes, guest memory, the
// calls into the libraries libhttp imports (liblv2's heaps and lightweight mutexes, cellHttpUtil, libnet, libfs, cellRtc,
// all pspocket's own), the checks every function makes of the IDs it is given, and the library's records.
//
// libhttp keeps everything in guest memory: its objects (clients, transactions, headers, connections, cookies) in
// liblv2 heaps made over the pools the game hands it (cellHttpInit's, cellHttpInitCookie's), its globals in its data
// segment. pspocket does the same, so the IDs (the objects' addresses), the pools' bytes and what the game reads back
// are libhttp's: every object is a heap block of libhttp's size made at the same moment, its fields at libhttp's
// offsets (the named offsets in each file); the globals are in a block of pspocket's own (Lib::data).

#include <initializer_list>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/types.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/ppu/context.h"

namespace pspocket::hle::http {

inline constexpr std::string_view kChan = "cellHttp";

// RPCS3 cellHttp.h (CellHttpError, CellHttpsError).
enum : u32 {
    CELL_HTTP_ERROR_ALREADY_INITIALIZED = 0x80710001,
    CELL_HTTP_ERROR_NOT_INITIALIZED = 0x80710002,
    CELL_HTTP_ERROR_NO_MEMORY = 0x80710003,
    CELL_HTTP_ERROR_NO_BUFFER = 0x80710004,
    CELL_HTTP_ERROR_NO_STRING = 0x80710005,
    CELL_HTTP_ERROR_INSUFFICIENT = 0x80710006,
    CELL_HTTP_ERROR_INVALID_URI = 0x80710007,
    CELL_HTTP_ERROR_INVALID_HEADER = 0x80710008,
    CELL_HTTP_ERROR_BAD_METHOD = 0x80710009,
    CELL_HTTP_ERROR_BAD_CLIENT = 0x80710010,
    CELL_HTTP_ERROR_BAD_TRANS = 0x80710011,
    CELL_HTTP_ERROR_NO_CONNECTION = 0x80710012,
    CELL_HTTP_ERROR_NO_REQUEST_SENT = 0x80710013,
    CELL_HTTP_ERROR_ALREADY_BUILT = 0x80710014,
    CELL_HTTP_ERROR_ALREADY_SENT = 0x80710015,
    CELL_HTTP_ERROR_NO_HEADER = 0x80710016,
    CELL_HTTP_ERROR_NO_CONTENT_LENGTH = 0x80710017,
    CELL_HTTP_ERROR_TOO_MANY_REDIRECTS = 0x80710018,
    CELL_HTTP_ERROR_TOO_MANY_AUTHS = 0x80710019,
    CELL_HTTP_ERROR_TRANS_NO_CONNECTION = 0x80710020,
    CELL_HTTP_ERROR_CB_FAILED = 0x80710021,
    CELL_HTTP_ERROR_NOT_PIPED = 0x80710022,
    CELL_HTTP_ERROR_OUT_OF_ORDER_PIPE = 0x80710023,
    CELL_HTTP_ERROR_TRANS_ABORTED = 0x80710024,
    CELL_HTTP_ERROR_BROKEN_PIPELINE = 0x80710025,
    CELL_HTTP_ERROR_UNAVAILABLE = 0x80710026,
    CELL_HTTP_ERROR_INVALID_VALUE = 0x80710027,
    CELL_HTTP_ERROR_CANNOT_AUTHENTICATE = 0x80710028,
    CELL_HTTP_ERROR_COOKIE_NOT_FOUND = 0x80710041,
    CELL_HTTP_ERROR_COOKIE_INVALID_DOMAIN = 0x80710042,
    CELL_HTTP_ERROR_CACHE_ALREADY_INITIALIZED = 0x80710043,
    CELL_HTTP_ERROR_CACHE_NOT_INITIALIZED = 0x80710044,
    CELL_HTTP_ERROR_LINE_EXCEEDS_MAX = 0x80710045,
    CELL_HTTP_ERROR_REQUIRES_BASIC_AUTH = 0x80710046,
    CELL_HTTP_ERROR_UNKNOWN = 0x80710051,
    CELL_HTTP_ERROR_INTERNAL = 0x80710052,
    CELL_HTTP_ERROR_NONREMOVABLE = 0x80710053,
    CELL_HTTP_ERROR_BAD_CONN = 0x80710054,
    CELL_HTTP_ERROR_BAD_MAN = 0x80710055,
    CELL_HTTP_ERROR_NO_POOL = 0x80710056,
    CELL_HTTP_ERROR_NO_REQUEST = 0x80710057,
    CELL_HTTP_ERROR_LOCK_FAILED = 0x80710058,
    CELL_HTTP_ERROR_INVALID_DATA = 0x80710059,
    CELL_HTTP_ERROR_BROKEN_CHUNK = 0x8071005A,
    CELL_HTTP_ERROR_DCACHE_NOT_INITIALIZED = 0x80710062,
    CELL_HTTP_ERROR_NET_RESOLVER = 0x80710100,
    CELL_HTTPS_ERROR_CERTIFICATE_LOAD = 0x80710A01,
    CELL_HTTPS_ERROR_BAD_MEMORY = 0x80710A02,
    CELL_HTTPS_ERROR_CONTEXT_CREATION = 0x80710A03,
    CELL_HTTPS_ERROR_NO_SSL = 0x80710A08,
    CELL_HTTPS_ERROR_KEY_LOAD = 0x80710A09,
    CELL_HTTPS_ERROR_CERT_KEY_MISMATCH = 0x80710A0A,
    CELL_HTTPS_ERROR_KEY_NEEDS_CERT = 0x80710A0B,
    CELL_HTTPS_ERROR_CERT_NEEDS_KEY = 0x80710A0C,
};
constexpr s32 err(u32 code) { return static_cast<s32>(code); }
constexpr s32 kOk = 0;

// ------------------------------------------------------------------------------------------------- guest memory

inline u8* gb(PPUContext& ctx) { return guest_base(ctx); }
inline u8 r8(PPUContext& ctx, u32 a) { return ps_read8(gb(ctx), a); }
inline u16 r16(PPUContext& ctx, u32 a) { return ps_read16(gb(ctx), a); }
inline u32 r32(PPUContext& ctx, u32 a) { return ps_read32(gb(ctx), a); }
inline u64 r64(PPUContext& ctx, u32 a) { return ps_read64(gb(ctx), a); }
inline void w8(PPUContext& ctx, u32 a, u8 v) { ps_write8(gb(ctx), a, v); }
inline void w16(PPUContext& ctx, u32 a, u16 v) { ps_write16(gb(ctx), a, v); }
inline void w32(PPUContext& ctx, u32 a, u32 v) { ps_write32(gb(ctx), a, v); }
inline void w64(PPUContext& ctx, u32 a, u64 v) { ps_write64(gb(ctx), a, v); }
void zero(PPUContext& ctx, u32 a, u32 size);
void copy(PPUContext& ctx, u32 dst, u32 src, u32 size); // memmove, as pspocket's _sys_memcpy
u32 str_len(PPUContext& ctx, u32 s);
std::string str(PPUContext& ctx, u32 s);
// liblv2's _sys_strncasecmp and _sys_strncmp over guest strings.
s32 str_ncasecmp(PPUContext& ctx, u32 a, u32 b, u32 n);
s32 str_ncmp(PPUContext& ctx, u32 a, u32 b, u32 n);

// MD5 (RFC 1321), which libhttp uses to file cookies and records.
void md5(const u8* data, usize len, u8 out[16]);

// A pointer the caller gave, checked before use (D-042): where libhttp would fault on memory that is not there, the
// call site is logged and the call ends with CELL_HTTP_ERROR_NO_BUFFER.
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what);

// ------------------------------------------------------------------------------------------------- other libraries

// The HLE function libhttp imports (module, name), called as libhttp calls it: r3 of the call, the caller's argument
// registers as they were.
u64 call(PPUContext& ctx, std::string_view module, std::string_view name, std::initializer_list<u64> args);
// The same for an import pspocket may not have (libnet's resolver functions known only by NID, D-523): as the loader's
// stub for a missing function answers (logged, CELL_ENOSYS) when the host lacks it.
u64 call_nid(PPUContext& ctx, std::string_view module, u32 nid, std::initializer_list<u64> args);
inline s32 util(PPUContext& ctx, std::string_view name, std::initializer_list<u64> args)
{
    return static_cast<s32>(call(ctx, "cellHttpUtil", name, args));
}
// An lv2 call as libhttp makes it (`sc`), through the kernel's dispatcher.
inline s32 sc(PPUContext& ctx, u32 number, std::initializer_list<u64> args)
{
    return static_cast<s32>(lv2_syscall(ctx, number, args));
}

// libhttp's own check of an object's address: sys_memory_get_page_attribute (351) on it, which fails for memory that is
// not there, then the object's pointer to itself at `self`.
bool page_ok(PPUContext& ctx, u32 addr);
bool valid(PPUContext& ctx, u32 obj, u32 self);
// The process's SDK version (sys_process_getpid, then sys_process_get_sdk_version), as libhttp asks for it.
u32 sdk_version(PPUContext& ctx);

// The lightweight mutexes libhttp makes (liblv2's): priority, not recursive, an 8-byte name.
s32 lw_create(PPUContext& ctx, u32 lw, std::string_view name);
s32 lw_destroy(PPUContext& ctx, u32 lw);
s32 lw_lock(PPUContext& ctx, u32 lw);
s32 lw_unlock(PPUContext& ctx, u32 lw);

// The lv2 mutex and condition libhttp's waits use ("_httsyn", syscalls 100/105, 101/106) at `sync`: {mutex, cond, u64
// value, u64}.
s32 sync_create(PPUContext& ctx, u32 sync, u64 value);
void sync_destroy(PPUContext& ctx, u32 sync);

// ------------------------------------------------------------------------------------------------- the library's state

// libhttp's globals (pspocket's block; offsets of libhttp's records within it).
struct Lib {
    u32 data = 0;
};
Lib& lib();
u32 data(PPUContext& ctx); // the block, laid out at first use

// The system record (libhttp's globals for cellHttpInit): the global proxy (a CellHttpUri), its pool, the lock, the
// heap made over the game's pool, the connections' buffer size, whether initialised and whether a proxy is set.
inline constexpr u32 kSys = 0x000;
inline constexpr u32 kSysProxy = 0, kSysProxyPool = 28, kSysLock = 32, kSysHeap = 56, kSysBufSize = 60, kSysInit = 64,
                     kSysProxySet = 65;
// The cookie store (cellHttpInitCookie's): its sizes, 16 domain buckets at +24, its lock at +88, its heap at +124.
inline constexpr u32 kCookie = 0x080;
// The authentication cache: its lock, the entry maximum at +24, count +28, list head +32 and tail +36, initialised +40.
inline constexpr u32 kAuth = 0x120;
// The redirection records ("_httrec"): the lock, 16 records of 96 bytes at +24, the free list +1560, the used list
// +1564 (head) and +1568 (tail), initialised +1572.
inline constexpr u32 kRec = 0x180;
// The HTTPS context's address (cellHttpsInit's 64-byte block in the heap).
inline constexpr u32 kHttps = 0x7C0;
// The table of transaction functions a manager is given (libhttp's: abort and release).
inline constexpr u32 kTransactionTable = 0x7D0;
// The caches (cellHttpInitCache's and the external one) and the disk cache.
inline constexpr u32 kCache = 0x800, kExtCache = 0x880, kDiskCache = 0x900;
// Strings libhttp hands to other libraries (heap names).
inline constexpr u32 kStrHeap = 0xC00, kStrCookieHeap = 0xC20;
inline constexpr u32 kDataSize = 0x1000;

bool initialized(PPUContext& ctx);
bool https_initialized(PPUContext& ctx);
// libhttp's malloc and free (liblv2's heap over cellHttpInit's pool).
u32 hmalloc(PPUContext& ctx, u32 size);
void hfree(PPUContext& ctx, u32 addr);

// ------------------------------------------------------------------------------------------------- objects

// The client (224 bytes): its connection manager +0, HTTP version +4/+8, user agent size +12 and string +16, headers
// +20 (count +24), the response buffer maximum +28, +32, transactions +36, the proxy (a CellHttpUri) +40 and its pool
// +68, callbacks +72..+124, the client's own +128, +136 (u64), +148, +156, the lock "_httcla" +160, the lock "_httcln"
// +184, the switches +208..+219.
namespace client {
inline constexpr u32 kSize = 224;
inline constexpr u32 kManager = 0, kMajor = 4, kMinor = 8, kUaSize = 12, kUa = 16, kHeaders = 20, kHeaderCount = 24,
                     kResponseBufferMax = 28, k32 = 32, kTransactions = 36, kProxy = 40, kProxyPool = 68,
                     kAuthCb = 72, kTransStateCb = 80, kRedirectCb = 88, kCookieSendCb = 96, kCookieRecvCb = 104,
                     kSslCb = 112, kSslIdDestroyCb = 120, kSelf = 128, k136 = 136, k148 = 148, k156 = 156,
                     kTransLock = 160, kLock = 184, kKeepAlive = 208, kAutoRedirect = 209, kAutoAuth = 210,
                     kPipeline = 211, kUseProxy = 212, kCookie = 213, k214 = 214, k215 = 215, kAuthCache = 216,
                     kCache = 217, k218 = 218, kDiskCache = 219;
} // namespace client

// The connection manager (168 bytes, one per client): timeouts (u64, microseconds) for receiving +0, sending +8,
// connecting +16 and waiting for a connection +24; the lock "_httcmg" +32; two host lists +56 and +84; the limits
// +64..+108; the connections +112; the connection state callback +120/+124; +132; the client +136; the SSL version
// +144; the buffer size +148; the receive and send buffer sizes +152/+156; the switches +160/+161; its own +140.
namespace manager {
inline constexpr u32 kSize = 168;
inline constexpr u32 kRecvTimeout = 0, kSendTimeout = 8, kConnTimeout = 16, kWaitTimeout = 24, kLock = 32,
                     kHosts = 56, kCount64 = 64, kTotalPool = 68, kPerHostPool = 72, kPerPipelineMax = 76,
                     kPerHostKeepAliveMax = 80, kHosts2 = 84, kCount92 = 92, k96 = 96, k100 = 100, k104 = 104,
                     k108 = 108, kConnections = 112, kAllocator = 116, kStateCb = 120, kCount128 = 128, k132 = 132,
                     kClient = 136, kSelf = 140, kSslVersion = 144, kBufSize = 148, kRecvBuf = 152, kSendBuf = 156,
                     kWaitStatus = 160, k161 = 161;
} // namespace manager

// A header list node (20 bytes): next, previous, the CellHttpHeader {name, value} at +8, its pool at +16.
namespace hnode {
inline constexpr u32 kSize = 20, kNext = 0, kPrev = 4, kHeader = 8, kName = 8, kValue = 12, kPool = 16;
} // namespace hnode

// Header lists (a head word's address): add a copy of a CellHttpHeader at the end, free every node, find one by name
// (its CellHttpHeader's address), copy all into a caller's array and pool.
s32 headers_add(PPUContext& ctx, u32 head, u32 header);
void headers_free(PPUContext& ctx, u32 first);
s32 headers_find(PPUContext& ctx, u32 first, u32 out, u32 name);
s32 headers_copy_all(PPUContext& ctx, u32 first, u32 headers, u32 pool, u32 size, u32 required);

// A proxy as cellHttpSetProxy and cellHttpClientSetProxy take it: the CellHttpUri's hostname parsed as a URI for its
// host, its other fields kept, the result copied into `uri` with a pool of its own; the parse's pool is the caller's
// to free once the copy is stored.
s32 proxy_prepare(PPUContext& ctx, u32 proxy, u32 uri, u32& pool, u32& parse_pool);

// Registration of each file's functions.
void register_core(Registry& r);
void register_clients(Registry& r);
void register_cookies(Registry& r);
void register_https(Registry& r);

// A connection (cellHttpConnection.cpp).
namespace conn {
inline constexpr u32 kSocket = 40, kSelf = 148;
} // namespace conn
bool connection_valid(PPUContext& ctx, u32 conn);
bool connection_flag140(PPUContext& ctx, u32 conn);
bool connection_flag141(PPUContext& ctx, u32 conn);
s32 connection_destroy(PPUContext& ctx, u32 conn, u32 ssl_record);
s32 hosts_clear(PPUContext& ctx, u32 list, u32 ssl_record);

// The connection manager (cellHttpClient.cpp).
s32 manager_create(PPUContext& ctx, u32 out, u32 total, u32 per_host, u32 keep_alive, u32 per_pipeline, u32 client);
s32 manager_destroy(PPUContext& ctx, u32 m);
s32 manager_close_all(PPUContext& ctx, u32 m);
s32 manager_ssl_version(PPUContext& ctx, u32 m, u32 out);

// Cookies (cellHttpCookie.cpp): the client's session cookies flushed when it is destroyed.
s32 session_cookie_flush(PPUContext& ctx, u32 client);
// HTTPS (cellHttps.cpp): a client's SSL record made and freed with the client, a socket's SSL object closed.
s32 client_ssl_create(PPUContext& ctx, u32 client);
s32 client_ssl_destroy(PPUContext& ctx, u32 client);
s32 client_ssl_ctx_free(PPUContext& ctx, u32 client);
void socket_ssl_close(PPUContext& ctx, u32 sock, u32 ssl_record);

} // namespace pspocket::hle::http
