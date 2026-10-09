// cellSsl, D-530: SSL-C's common layer as libssl.sprx (4.92) has it: its memory functions over the heap cellSslInit makes,
// the lock callback, LHASH, the object table and OBJ_NAME, the error queue (ERR), the library context and its resource
// list, the random generator's seeding. Read from libssl's code and compared with it in tests/hle/test_ssl_compare.cpp.
// These are OpenSSL 0.9's lhash.c, o_names.c and err.c in SSLeay's form, with RSA's changes (a realloc that takes the
// old size, an LHASH that grows before it inserts, an ERR_STATE whose buffers are not cleared).

#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>

#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"
#include "ssl.hpp"

namespace pspocket::hle::ssl {

// ------------------------------------------------------------------------------------------------- guest memory

void zero(PPUContext& ctx, u32 a, u32 size)
{
    if (size) std::memset(gb(ctx) + a, 0, size);
}

void copy(PPUContext& ctx, u32 dst, u32 src, u32 size)
{
    if (size) std::memmove(gb(ctx) + dst, gb(ctx) + src, size);
}

void put(PPUContext& ctx, u32 dst, const void* src, u32 size)
{
    if (size) std::memcpy(gb(ctx) + dst, src, size);
}

std::vector<u8> bytes(PPUContext& ctx, u32 a, u32 size)
{
    return std::vector<u8>(gb(ctx) + a, gb(ctx) + a + size);
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

s32 str_cmp(PPUContext& ctx, u32 a, u32 b)
{
    const u8* g = gb(ctx);
    for (u32 i = 0;; i++) {
        const int x = g[a + i], y = g[b + i];
        if (x != y) return x - y;
        if (!x) return 0;
    }
}

s32 mem_cmp(PPUContext& ctx, u32 a, u32 b, u32 n)
{
    const u8* g = gb(ctx);
    for (u32 i = 0; i < n; i++) {
        if (g[a + i] != g[b + i]) return static_cast<int>(g[a + i]) - static_cast<int>(g[b + i]);
    }
    return 0;
}

bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    lv2::PpuThread& t = thread_of(ctx);
    bool ok = u64{addr} + size <= GuestMemory::kGuestSize;
    const u64 page = GuestMemory::host_page_size();
    for (u64 a = addr & ~(page - 1); ok && size && a < u64{addr} + size; a += page)
        ok = t.kernel.memory().is_committed(static_cast<u32>(a));
    if (!ok) {
        log_call_site(ctx, gb(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory (libssl faults)", what, addr,
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

} // namespace

u64 call(PPUContext& ctx, std::string_view module, std::string_view name, std::initializer_list<u64> args)
{
    const Function* f = find(module, recomp::nid_of(name));
    if (!f) PS_FATAL(kChan, "{}::{}, which libssl imports, is not pspocket's", module, name);
    std::array<u64, 8> saved{};
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 0;
    for (const u64 a : args) ctx.gpr[3 + i++] = a;
    f->fn(ctx, gb(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

bool page_ok(PPUContext& ctx, u32 addr)
{
    StackVar attr(ctx, gb(ctx), 24); // sys_page_attr_t
    return sc(ctx, 351, {addr, attr.addr()}) == 0;
}

// ------------------------------------------------------------------------------------------------- the library's state

Lib& lib() { return HleProcess::current()->state<Lib>(); }

namespace {

// The block: the function ids first (4 bytes each), the lwmutex, then what lit() and object() hand out.
constexpr u32 kFnIds = 0x000, kLwmutex = 0x100, kHeapName = 0x120, kFirstFree = 0x140;

u32 block(PPUContext& ctx)
{
    Lib& l = lib();
    if (!l.block) {
        l.block = HleProcess::current()->main_memory().alloc(kGlobalsSize, 0x80);
        if (!l.block) PS_FATAL(kChan, "no guest memory for libssl's globals");
        zero(ctx, l.block, kGlobalsSize);
        l.block_used = kFirstFree;
        l.lwmutex = l.block + kLwmutex;
        std::memcpy(gb(ctx) + l.block + kHeapName, "cellSsl_Library", 16);
    }
    return l.block;
}

u32 block_alloc(PPUContext& ctx, u32 size, u32 align)
{
    Lib& l = lib();
    block(ctx);
    const u32 at = (l.block_used + align - 1) & ~(align - 1);
    if (at + size > kGlobalsSize) PS_FATAL(kChan, "libssl's globals block is full");
    l.block_used = at + size;
    return l.block + at;
}

} // namespace

u32 globals_block() { return lib().block; }

u32 fn(PPUContext& ctx, Fn f) { return block(ctx) + kFnIds + 4 * static_cast<u32>(f); }

u32 lit(PPUContext& ctx, std::string_view text)
{
    Lib& l = lib();
    for (const auto& [t, at] : l.literals)
        if (t == text) return at;
    const u32 at = block_alloc(ctx, static_cast<u32>(text.size()) + 1, 1);
    put(ctx, at, text.data(), static_cast<u32>(text.size()));
    l.literals.emplace_back(std::string(text), at);
    return at;
}

u32 object(PPUContext& ctx, std::string_view name, u32 size)
{
    Lib& l = lib();
    const std::string key = std::string("\x01") + std::string(name);
    for (const auto& [t, at] : l.literals)
        if (t == key) return at;
    const u32 at = block_alloc(ctx, size ? size : 4, 8);
    l.literals.emplace_back(key, at);
    return at;
}

u32 heap_name(PPUContext& ctx) { return block(ctx) + kHeapName; }

// ------------------------------------------------------------------------------------------------- memory, locks

u32 Malloc(PPUContext& ctx, u32 size)
{
    Lib& l = lib();
    if (!l.mem_funcs) {
        log_call_site(ctx, gb(ctx), "an allocation before cellSslInit (libssl calls its memory function, which is null "
                                    "then, and faults): none");
        return 0;
    }
    const u32 m = static_cast<u32>(call(ctx, "sysPrxForUser", "_sys_heap_malloc", {l.heap, size}));
    if (m) return m;
    // libssl's malloc on failure: the heap's free size and mallinfo, to the game's callback when it set one.
    const u32 free_size = static_cast<u32>(call(ctx, "sysPrxForUser", "_sys_heap_get_total_free_size", {l.heap}));
    StackVar mi(ctx, gb(ctx), 40);
    call(ctx, "sysPrxForUser", "_sys_heap_get_mallinfo", {l.heap, mi.addr()});
    if (l.mem_cb) {
        StackVar info(ctx, gb(ctx), 40);
        copy(ctx, info.addr(), mi.addr(), 40);
        call_guest(ctx, l.mem_cb, {free_size, size, info.addr(), l.mem_cb_arg});
    }
    return 0;
}

u32 Realloc(PPUContext& ctx, u32 p, u32 size, u32 old_size)
{
    Lib& l = lib();
    if (!l.mem_funcs) {
        log_call_site(ctx, gb(ctx), "a reallocation before cellSslInit (libssl faults): none");
        return 0;
    }
    // libssl's realloc (it takes the old size): a new block, the smaller size copied, the old one freed.
    if (size) {
        const u32 m = static_cast<u32>(call(ctx, "sysPrxForUser", "_sys_heap_malloc", {l.heap, size}));
        if (!m) return 0;
        if (p) {
            copy(ctx, m, p, size < old_size ? size : old_size);
            call(ctx, "sysPrxForUser", "_sys_heap_free", {l.heap, p});
        }
        return m;
    }
    if (p) call(ctx, "sysPrxForUser", "_sys_heap_free", {l.heap, p});
    return 0;
}

void Free(PPUContext& ctx, u32 p)
{
    Lib& l = lib();
    if (!l.mem_funcs) {
        log_call_site(ctx, gb(ctx), "a free before cellSslInit (libssl faults): ignored");
        return;
    }
    call(ctx, "sysPrxForUser", "_sys_heap_free", {l.heap, p});
}

void crypto_lock(PPUContext& ctx, u32 mode)
{
    Lib& l = lib();
    if (!l.lock_cb) return;
    if (mode & 1) call(ctx, "sysPrxForUser", "sys_lwmutex_lock", {l.lwmutex, 0});
    else call(ctx, "sysPrxForUser", "sys_lwmutex_unlock", {l.lwmutex});
}

// ------------------------------------------------------------------------------------------------- LHASH

namespace {

enum Lh : u32 {
    kB = 0, kComp = 4, kHash = 8, kNumNodes = 12, kNumAlloc = 16, kP = 20, kPmax = 24, kUpLoad = 28, kDownLoad = 32,
    kNumItems = 36, kNumExpands = 40, kNumExpandReallocs = 44, kNumContracts = 48, kNumContractReallocs = 52,
    kNumHashCalls = 56, kNumCompCalls = 60, kNumInsert = 64, kNumReplace = 68, kNumDelete = 72, kNumNoDelete = 76,
    kNumRetrieve = 80, kNumRetrieveMiss = 84, kNumHashComps = 88, kError = 92, kLhSize = 96,
};

void inc(PPUContext& ctx, u32 a) { w32(ctx, a, r32(ctx, a) + 1); }

u32 call_hash(PPUContext& ctx, u32 lh, u32 data)
{
    const u32 h = r32(ctx, lh + kHash);
    if (h == fn(ctx, Fn::ObjNameHash)) {
        // OBJ_NAME's: the string hash of the name, XORed with the type (no type has a hash function of its own).
        return lh_strhash(ctx, r32(ctx, data + 8)) ^ r32(ctx, data);
    }
    if (h == fn(ctx, Fn::ErrStateHash)) return r32(ctx, data) * 13; // the ERR state's thread ID
    PS_FATAL(kChan, "LHASH {:#x}: unknown hash function {:#x}", lh, h);
}

s32 call_comp(PPUContext& ctx, u32 lh, u32 a, u32 b)
{
    const u32 c = r32(ctx, lh + kComp);
    if (c == fn(ctx, Fn::ObjNameCmp)) {
        const s32 d = static_cast<s32>(r32(ctx, a) - r32(ctx, b));
        return d ? d : str_cmp(ctx, r32(ctx, a + 8), r32(ctx, b + 8));
    }
    if (c == fn(ctx, Fn::ErrStateCmp)) return static_cast<s32>(r32(ctx, a) - r32(ctx, b));
    PS_FATAL(kChan, "LHASH {:#x}: unknown compare function {:#x}", lh, c);
}

// getrn: the address of the word that holds (or would hold) the node for `data`; its hash in `hash`.
u32 getrn(PPUContext& ctx, u32 lh, u32 data, u32& hash)
{
    hash = call_hash(ctx, lh, data);
    inc(ctx, lh + kNumHashCalls);
    u32 nn = hash % r32(ctx, lh + kPmax);
    if (nn < r32(ctx, lh + kP)) nn = hash % r32(ctx, lh + kNumAlloc);
    u32 ret = r32(ctx, lh + kB) + 4 * nn;
    for (u32 n = r32(ctx, ret); n; n = r32(ctx, n + 4)) {
        inc(ctx, lh + kNumHashComps);
        if (r32(ctx, n + 8) == hash) {
            inc(ctx, lh + kNumCompCalls);
            if (call_comp(ctx, lh, r32(ctx, n), data) == 0) break;
        }
        ret = n + 4;
    }
    return ret;
}

} // namespace

u32 lh_strhash(PPUContext& ctx, u32 s)
{
    if (!s || !r8(ctx, s)) return 0;
    u32 ret = 0, n = 0x100;
    for (u32 i = 0; r8(ctx, s + i); i++) {
        const u32 v = n | r8(ctx, s + i);
        n += 0x100;
        const u32 r = ((v >> 2) ^ v) & 0x0F;
        ret = r ? (ret << r) | (ret >> (32 - r)) : ret;
        ret ^= v * v;
    }
    return (ret >> 16) ^ ret;
}

u32 lh_new(PPUContext& ctx, u32 hash_fn, u32 comp_fn)
{
    const u32 lh = Malloc(ctx, kLhSize);
    if (!lh) return 0;
    const u32 b = Malloc(ctx, 0x40);
    if (!b) {
        Free(ctx, lh);
        return 0;
    }
    zero(ctx, b, 0x40);
    w32(ctx, lh + kB, b);
    w32(ctx, lh + kComp, comp_fn);
    w32(ctx, lh + kHash, hash_fn);
    w32(ctx, lh + kNumNodes, 8);
    w32(ctx, lh + kNumAlloc, 16);
    w32(ctx, lh + kP, 0);
    w32(ctx, lh + kPmax, 8);
    w32(ctx, lh + kUpLoad, 0x200);
    w32(ctx, lh + kDownLoad, 0x100);
    zero(ctx, lh + kNumItems, kLhSize - kNumItems);
    return lh;
}

u32 lh_insert(PPUContext& ctx, u32 lh, u32 data)
{
    w32(ctx, lh + kError, 0);
    // Grown first when full (libssl reallocates before it splits a bucket).
    if (((r32(ctx, lh + kNumItems) & 0xFFFFFF) << 8) / r32(ctx, lh + kNumNodes) >= r32(ctx, lh + kUpLoad)) {
        u32 nni = 0;
        bool grow = true;
        if (r32(ctx, lh + kP) + 1 >= r32(ctx, lh + kPmax)) {
            const u32 n = r32(ctx, lh + kNumAlloc);
            nni = n * 2;
            const u32 nb = Realloc(ctx, r32(ctx, lh + kB), n * 8, n * 4);
            if (nb) {
                for (u32 i = n; i < nni; i++) w32(ctx, nb + 4 * i, 0);
                w32(ctx, lh + kB, nb);
            } else {
                inc(ctx, lh + kError);
                grow = false;
            }
        }
        if (grow) {
            const u32 p = r32(ctx, lh + kP), pmax = r32(ctx, lh + kPmax), alloc = r32(ctx, lh + kNumAlloc);
            const u32 b = r32(ctx, lh + kB);
            u32 n1 = b + 4 * p;
            const u32 n2 = b + 4 * (p + pmax);
            inc(ctx, lh + kNumNodes);
            w32(ctx, n2, 0);
            inc(ctx, lh + kNumExpands);
            w32(ctx, lh + kP, p + 1);
            for (u32 np = r32(ctx, n1); np; np = r32(ctx, n1)) {
                if (r32(ctx, np + 8) % alloc == p) {
                    n1 = np + 4;
                } else {
                    w32(ctx, n1, r32(ctx, np + 4));
                    w32(ctx, np + 4, r32(ctx, n2));
                    w32(ctx, n2, np);
                }
            }
            if (r32(ctx, lh + kPmax) <= r32(ctx, lh + kP)) {
                w32(ctx, lh + kPmax, r32(ctx, lh + kNumAlloc));
                w32(ctx, lh + kNumAlloc, nni);
                inc(ctx, lh + kNumExpandReallocs);
                w32(ctx, lh + kP, 0);
            }
        }
        if (r32(ctx, lh + kError)) return 0;
    }
    u32 hash = 0;
    const u32 rn = getrn(ctx, lh, data, hash);
    const u32 node = r32(ctx, rn);
    if (!node) {
        const u32 nn = Malloc(ctx, 12);
        if (!nn) {
            inc(ctx, lh + kError);
            return 0;
        }
        w32(ctx, nn, data);
        w32(ctx, nn + 4, 0);
        w32(ctx, nn + 8, hash);
        w32(ctx, rn, nn);
        inc(ctx, lh + kNumItems);
        inc(ctx, lh + kNumInsert);
        return 0;
    }
    inc(ctx, lh + kNumReplace);
    const u32 old = r32(ctx, node);
    w32(ctx, node, data);
    return old;
}

u32 lh_delete(PPUContext& ctx, u32 lh, u32 data)
{
    w32(ctx, lh + kError, 0);
    u32 hash = 0;
    const u32 rn = getrn(ctx, lh, data, hash);
    const u32 node = r32(ctx, rn);
    if (!node) {
        inc(ctx, lh + kNumNoDelete);
        return 0;
    }
    const u32 ret = r32(ctx, node);
    w32(ctx, rn, r32(ctx, node + 4));
    Free(ctx, node);
    inc(ctx, lh + kNumDelete);
    const u32 items = r32(ctx, lh + kNumItems) - 1;
    w32(ctx, lh + kNumItems, items);
    const u32 nodes = r32(ctx, lh + kNumNodes);
    if (nodes > 16 && ((items & 0xFFFFFF) << 8) / nodes <= r32(ctx, lh + kDownLoad)) {
        // contract: the last bucket's nodes go to the end of the bucket that takes it in.
        const u32 b = r32(ctx, lh + kB);
        const u32 last = b + 4 * (r32(ctx, lh + kP) + r32(ctx, lh + kPmax) - 1);
        const u32 np = r32(ctx, last);
        w32(ctx, last, 0);
        if (r32(ctx, lh + kP) == 0) {
            const u32 alloc = r32(ctx, lh + kNumAlloc);
            const u32 nb = Realloc(ctx, b, (alloc & 0x7FFFFFFE) << 1, alloc << 2);
            if (!nb) {
                inc(ctx, lh + kError);
                return ret;
            }
            inc(ctx, lh + kNumContractReallocs);
            w32(ctx, lh + kNumAlloc, alloc >> 1);
            w32(ctx, lh + kP, (r32(ctx, lh + kPmax) >> 1) - 1);
            w32(ctx, lh + kB, nb);
            w32(ctx, lh + kPmax, r32(ctx, lh + kPmax) >> 1);
        } else {
            w32(ctx, lh + kP, r32(ctx, lh + kP) - 1);
        }
        w32(ctx, lh + kNumNodes, r32(ctx, lh + kNumNodes) - 1);
        inc(ctx, lh + kNumContracts);
        u32 n1 = r32(ctx, lh + kB) + 4 * r32(ctx, lh + kP);
        while (r32(ctx, n1)) n1 = r32(ctx, n1) + 4;
        w32(ctx, n1, np);
    }
    return ret;
}

u32 lh_retrieve(PPUContext& ctx, u32 lh, u32 data)
{
    w32(ctx, lh + kError, 0);
    u32 hash = 0;
    const u32 node = r32(ctx, getrn(ctx, lh, data, hash));
    if (!node) {
        inc(ctx, lh + kNumRetrieveMiss);
        return 0;
    }
    inc(ctx, lh + kNumRetrieve);
    return r32(ctx, node);
}

void lh_free(PPUContext& ctx, u32 lh)
{
    for (u32 i = 0; i < r32(ctx, lh + kNumNodes); i++) {
        for (u32 n = r32(ctx, r32(ctx, lh + kB) + 4 * i); n;) {
            const u32 next = r32(ctx, n + 4);
            Free(ctx, n);
            n = next;
        }
    }
    Free(ctx, r32(ctx, lh + kB));
    Free(ctx, lh);
}

// ------------------------------------------------------------------------------------------------- objects, names

namespace {

// RSA's object numbers and their names, as SSL-C numbers them (the first 127 are SSLeay's numbering); the ones libssl
// registers as cipher and digest names. OIDs and names from the standards that define them (PKCS #1, RFC 3279, NIST).
struct Obj {
    u32 nid;
    const char* sn;
    const char* ln;
};
constexpr Obj kObjs[] = {
    {3, "MD2", "md2"},
    {4, "MD5", "md5"},
    {5, "RC4", "rc4"},
    {7, "RSA-MD2", "md2WithRSAEncryption"},
    {8, "RSA-MD5", "md5WithRSAEncryption"},
    {44, "DES-EDE3-CBC", "des-ede3-cbc"},
    {64, "SHA1", "sha1"},
    {65, "RSA-SHA1", "sha1WithRSAEncryption"},
    {130, "RSA-SHA256", "sha256WithRSAEncryption"},
    {131, "RSA-SHA384", "sha384WithRSAEncryption"},
    {132, "RSA-SHA512", "sha512WithRSAEncryption"},
    {142, "AES128-CBC", "aes128-cbc"},
    {150, "AES256-CBC", "aes256-cbc"},
    {162, "SHA256", "sha256"},
    {163, "SHA384", "sha384"},
    {164, "SHA512", "sha512"},
};

const Obj* obj(u32 nid)
{
    for (const Obj& o : kObjs)
        if (o.nid == nid) return &o;
    return nullptr;
}

} // namespace

u32 obj_nid2sn(PPUContext& ctx, u32 nid)
{
    const Obj* o = obj(nid);
    if (!o) PS_FATAL(kChan, "OBJ_nid2sn({}): not in pspocket's object table", nid);
    return lit(ctx, o->sn);
}

u32 obj_nid2ln(PPUContext& ctx, u32 nid)
{
    const Obj* o = obj(nid);
    if (!o) PS_FATAL(kChan, "OBJ_nid2ln({}): not in pspocket's object table", nid);
    return lit(ctx, o->ln);
}

s32 obj_name_add(PPUContext& ctx, u32 name, u32 type, u32 data)
{
    Lib& l = lib();
    if (!l.obj_names) {
        l.obj_names = lh_new(ctx, fn(ctx, Fn::ObjNameHash), fn(ctx, Fn::ObjNameCmp));
        if (!l.obj_names) return 0;
    }
    const u32 onp = Malloc(ctx, 16);
    if (!onp) return 0;
    w32(ctx, onp, (type & 0xFFFF0000) | (type & 0x7FFF));
    w32(ctx, onp + 4, type & 0x8000);
    w32(ctx, onp + 8, name);
    w32(ctx, onp + 12, data);
    const u32 old = lh_insert(ctx, l.obj_names, onp);
    if (old) {
        Free(ctx, old);
        return 1;
    }
    if (!r32(ctx, l.obj_names + kError)) return 1;
    Free(ctx, onp);
    return 0;
}

namespace {

// OBJ_NAME_remove(name, type).
void obj_name_remove(PPUContext& ctx, u32 name, u32 type)
{
    Lib& l = lib();
    if (!l.obj_names) return;
    StackVar key(ctx, gb(ctx), 16);
    w32(ctx, key.addr(), (type & 0xFFFF0000) | (type & 0x7FFF));
    w32(ctx, key.addr() + 8, name);
    const u32 ret = lh_delete(ctx, l.obj_names, key.addr());
    if (ret) Free(ctx, ret);
}

} // namespace

void obj_name_cleanup(PPUContext& ctx, s32 type)
{
    Lib& l = lib();
    const u32 lh = l.obj_names;
    if (!lh) return;
    const u32 down_load = r32(ctx, lh + kDownLoad);
    w32(ctx, lh + kDownLoad, 0);
    l.obj_names_cleanup_type = type;
    // lh_doall reads the bucket array afresh for each bucket (a deletion can shrink it).
    for (s32 i = static_cast<s32>(r32(ctx, lh + kNumNodes)) - 1; i >= 0; i--) {
        for (u32 n = r32(ctx, r32(ctx, lh + kB) + 4 * static_cast<u32>(i)); n;) {
            const u32 onp = r32(ctx, n), next = r32(ctx, n + 4);
            if (l.obj_names_cleanup_type < 0 || static_cast<u32>(l.obj_names_cleanup_type) == r32(ctx, onp))
                obj_name_remove(ctx, r32(ctx, onp + 8), r32(ctx, onp));
            n = next;
        }
    }
    if (type < 0) {
        lh_free(ctx, lh);
        l.obj_names = 0;
    } else {
        w32(ctx, lh + kDownLoad, down_load);
    }
}

// ------------------------------------------------------------------------------------------------- errors

namespace {

// ERR_STATE (332 bytes): the thread ID +0, err_buffer[16] +4, err_data[16] +68, err_data_flags[16] +132,
// err_file[16] +196, err_line[16] +260, top +324, bottom +328.
enum ErrState : u32 { kEsPid = 0, kEsBuffer = 4, kEsData = 0x44, kEsFlags = 0x84, kEsFile = 0xC4, kEsLine = 0x104,
    kEsTop = 0x144, kEsBottom = 0x148, kEsSize = 0x14C };

// CRYPTO_thread_id: no ID callback is ever set, so every thread is 0.
constexpr u32 kThreadId = 0;

void err_state_free(PPUContext& ctx, u32 es)
{
    for (u32 i = 0; i < 16; i++) {
        const u32 d = r32(ctx, es + kEsData + 4 * i);
        if (d && (r32(ctx, es + kEsFlags + 4 * i) & 1)) {
            Free(ctx, d);
            w32(ctx, es + kEsData + 4 * i, 0);
        }
        w32(ctx, es + kEsFlags + 4 * i, 0);
    }
    Free(ctx, es);
}

// The fallback ERR_STATE (libssl's static one) when none can be made.
u32 err_fallback(PPUContext& ctx) { return object(ctx, "ERR_STATE fallback", kEsSize); }

} // namespace

u32 err_get_state(PPUContext& ctx)
{
    Lib& l = lib();
    StackVar key(ctx, gb(ctx), kEsSize);
    w32(ctx, key.addr(), kThreadId);
    crypto_lock(ctx, 5);
    if (!l.err_states) {
        crypto_lock(ctx, 6);
    } else {
        const u32 ret = lh_retrieve(ctx, l.err_states, key.addr());
        crypto_lock(ctx, 6);
        if (ret) return ret;
    }
    crypto_lock(ctx, 9);
    if (!l.err_states) {
        l.err_states = lh_new(ctx, fn(ctx, Fn::ErrStateHash), fn(ctx, Fn::ErrStateCmp));
        if (!l.err_states) {
            crypto_lock(ctx, 10);
            return err_fallback(ctx);
        }
    }
    const u32 es = Malloc(ctx, kEsSize);
    if (!es) {
        crypto_lock(ctx, 10);
        return err_fallback(ctx);
    }
    // Only the ID, the data, their flags and the indices are set; the codes, files and lines keep the block's bytes.
    w32(ctx, es + kEsPid, kThreadId);
    w32(ctx, es + kEsTop, 0);
    w32(ctx, es + kEsBottom, 0);
    for (u32 i = 0; i < 16; i++) {
        w32(ctx, es + kEsFlags + 4 * i, 0);
        w32(ctx, es + kEsData + 4 * i, 0);
    }
    const u32 old = lh_insert(ctx, l.err_states, es);
    crypto_lock(ctx, 10);
    if (old) err_state_free(ctx, old);
    return es;
}

void put_error(PPUContext& ctx, u32 libno, u32 func, u32 reason, std::string_view file, u32 line)
{
    const usize slash = file.rfind('/');
    const u32 base_name = lit(ctx, slash == std::string_view::npos ? file : file.substr(slash + 1));
    const u32 es = err_get_state(ctx);
    const u32 top = (r32(ctx, es + kEsTop) + 1) % 16;
    w32(ctx, es + kEsTop, top);
    if (top == r32(ctx, es + kEsBottom)) w32(ctx, es + kEsBottom, (top + 1) % 16);
    const u32 i = 4 * top;
    w32(ctx, es + kEsFile + i, base_name);
    w32(ctx, es + kEsBuffer + i, libno << 24 | (func & 0xFFF) << 12 | (reason & 0xFFF));
    w32(ctx, es + kEsLine + i, line);
    const u32 d = r32(ctx, es + kEsData + i);
    if (d && (r32(ctx, es + kEsFlags + i) & 1)) {
        Free(ctx, d);
        w32(ctx, es + kEsData + i, 0);
    }
    w32(ctx, es + kEsFlags + i, 0);
}

u32 err_get_error(PPUContext& ctx, bool take)
{
    const u32 es = err_get_state(ctx);
    const u32 bottom = r32(ctx, es + kEsBottom);
    if (bottom == r32(ctx, es + kEsTop)) return 0;
    const u32 i = (bottom + 1) % 16;
    const u32 ret = r32(ctx, es + kEsBuffer + 4 * i);
    if (take) {
        w32(ctx, es + kEsBottom, i);
        w32(ctx, es + kEsBuffer + 4 * i, 0);
    }
    return ret;
}

void err_clear(PPUContext& ctx)
{
    const u32 es = err_get_state(ctx);
    w32(ctx, es + kEsTop, 0);
    w32(ctx, es + kEsBottom, 0);
}

void err_remove_state(PPUContext& ctx)
{
    Lib& l = lib();
    StackVar key(ctx, gb(ctx), kEsSize);
    w32(ctx, key.addr(), kThreadId);
    crypto_lock(ctx, 9);
    if (!l.err_states) {
        crypto_lock(ctx, 10);
        return;
    }
    const u32 es = lh_delete(ctx, l.err_states, key.addr());
    if (r32(ctx, l.err_states + kNumItems) == 0) {
        lh_free(ctx, l.err_states);
        l.err_states = 0;
    }
    crypto_lock(ctx, 10);
    if (es) err_state_free(ctx, es);
}

void err_free_strings(PPUContext& ctx)
{
    Lib& l = lib();
    crypto_lock(ctx, 9);
    if (l.err_strings) {
        lh_free(ctx, l.err_strings);
        l.err_strings = 0;
    }
    crypto_lock(ctx, 10);
}

namespace {

// ERR_peek_error, ERR_get_error, ERR_clear_error.
u32 ERR_peek_error(PPUContext& ctx) { return err_get_error(ctx, false); }
u32 ERR_get_error(PPUContext& ctx) { return err_get_error(ctx, true); }
void ERR_clear_error(PPUContext& ctx) { err_clear(ctx); }

// The error strings: libssl loads none, so every lookup finds nothing (under the lock, as libssl looks).
u32 error_string_lookup(PPUContext& ctx)
{
    crypto_lock(ctx, 5);
    crypto_lock(ctx, 6);
    return 0;
}

// ERR_func_error_string(e): the function's name, which libssl doesn't have.
u32 ERR_func_error_string(PPUContext& ctx, u32 e)
{
    (void)e;
    return error_string_lookup(ctx);
}

// ERR_error_string(e, buf): "error:%08lX:" and the library, function and reason by number (liblv2's _sys_vsprintf),
// in buf, or in libssl's own 256-byte buffer.
u32 ERR_error_string(PPUContext& ctx, u32 e, u32 buf)
{
    error_string_lookup(ctx); // the library's name
    error_string_lookup(ctx); // the function's
    error_string_lookup(ctx); // the reason's
    if (!buf) buf = object(ctx, "ERR_error_string buffer", 256);
    const std::string text = std::format("error:{:08X}::lib({}) :func({}) :reason({})", e, (e >> 24) & 0xFF,
        (e >> 12) & 0xFFF, e & 0xFFF);
    if (!usable(ctx, buf, static_cast<u32>(text.size()) + 1, "ERR_error_string: buf")) return buf;
    put(ctx, buf, text.c_str(), static_cast<u32>(text.size()) + 1);
    return buf;
}

} // namespace

// ------------------------------------------------------------------------------------------------- the library context

namespace {

// The library context (380 bytes): the resource method table +0, the resource list +4, a slot pair for each resource
// type (an object +8 + 8 * type, data +12 + 8 * type), flags +376.
constexpr u32 kCtxSize = 0x17C, kCtxMethods = 0, kCtxList = 4, kCtxFlags = 0x178, kTypes = 0x2E;

// The digests and ciphers libssl's resource list registers (EVP_add_cipher, EVP_add_digest), in its order.
struct Digest {
    const char* object;
    u32 type, pkey_type;
};
constexpr u32 kCiphers[] = {5, 44, 142, 150};
constexpr Digest kDigests[] = {
    {"EVP_md2", 3, 0}, {"EVP_md2 RSA", 3, 7}, {"EVP_md5", 4, 0}, {"EVP_md5 RSA", 4, 8}, {"EVP_sha1", 64, 65},
    {"EVP_sha1 2", 64, 65}, {"EVP_sha256", 162, 130}, {"EVP_sha384", 163, 131}, {"EVP_sha512", 164, 132},
};

// The aliases the first resource adds (EVP_add_digest_alias: type 0x8001; EVP_add_cipher_alias: 0x8002).
struct Alias {
    const char* name;
    const char* alias;
    u32 type;
};
constexpr Alias kAliases[] = {
    {"MD5", "ssl2-md5", 0x8001}, {"MD5", "ssl3-md5", 0x8001}, {"SHA1", "ssl3-sha1", 0x8001},
    {"RSA-SHA1", "RSA-SHA1-2", 0x8001}, {"DSA-SHA1", "DSA-SHA1-old", 0x8001}, {"DSA-SHA1", "DSS1", 0x8001},
    {"DSA-SHA1", "dss1", 0x8001}, {"DES-CBC", "des", 0x8002}, {"DES-CBC", "DES", 0x8002},
    {"DES-EDE3-CBC", "des3", 0x8002}, {"DES-EDE3-CBC", "DES3", 0x8002}, {"RC2-CBC", "rc2", 0x8002},
    {"RC2-CBC", "RC2", 0x8002}, {"RC5-CBC", "rc5", 0x8002}, {"RC5-CBC", "RC5", 0x8002},
    {"RIPEMD160", "ripemd", 0x8001}, {"RIPEMD160", "rmd160", 0x8001}, {"DESX-CBC", "desx", 0x8002},
    {"DESX-CBC", "DESX", 0x8002}, {"IDEA-CBC", "IDEA", 0x8002}, {"IDEA-CBC", "idea", 0x8002},
    {"BF-CBC", "BF", 0x8002}, {"BF-CBC", "bf", 0x8002}, {"BF-CBC", "blowfish", 0x8002},
    {"CAST5-CBC", "CAST", 0x8002}, {"CAST5-CBC", "cast", 0x8002}, {"CAST5-CBC", "CAST-cbc", 0x8002},
    {"CAST5-CBC", "cast-cbc", 0x8002}, {"RC5-CBC", "rc5", 0x8002}, {"RC5-CBC", "RC5", 0x8002},
};

void evp_add_cipher(PPUContext& ctx, u32 nid)
{
    const u32 c = object(ctx, std::format("EVP_CIPHER {}", nid), 0x40);
    if (obj_name_add(ctx, obj_nid2sn(ctx, nid), 2, c)) obj_name_add(ctx, obj_nid2ln(ctx, nid), 2, c);
}

void evp_add_digest(PPUContext& ctx, const Digest& d)
{
    const u32 md = object(ctx, d.object, 0x40);
    const u32 name = obj_nid2sn(ctx, d.type);
    if (!obj_name_add(ctx, name, 1, md)) return;
    if (!obj_name_add(ctx, obj_nid2ln(ctx, d.type), 1, md)) return;
    if (d.pkey_type && d.pkey_type != d.type) {
        if (obj_name_add(ctx, obj_nid2sn(ctx, d.pkey_type), 0x8001, name))
            obj_name_add(ctx, obj_nid2ln(ctx, d.pkey_type), 0x8001, name);
    }
}

} // namespace

s32 libctx_new(PPUContext& ctx, u32& out)
{
    Lib& l = lib();
    out = 0;
    const u32 c = Malloc(ctx, kCtxSize);
    if (!c) return 0x2715;
    zero(ctx, c, kCtxSize);
    w32(ctx, c + kCtxList, fn(ctx, Fn::ResourceList));
    // The resources, in the list's order. The first: the cipher and digest aliases, the time and RSA callbacks.
    for (const Alias& a : kAliases) obj_name_add(ctx, lit(ctx, a.alias), a.type, lit(ctx, a.name));
    l.time_cb = true;
    // The BIO methods, the default RSA and DH methods, four that do nothing (statics only), then the ciphers and
    // digests.
    for (const u32 nid : kCiphers) evp_add_cipher(ctx, nid);
    for (const Digest& d : kDigests) evp_add_digest(ctx, d);
    // The X509 methods (SSLCERT_*'s implementation), the random generator (no init), the resource methods.
    l.x509_methods = true;
    w32(ctx, c + kCtxMethods, fn(ctx, Fn::LibMethods));
    out = c;
    // Then the context's lookup of resource 6/0x12, which the list doesn't have: nothing more.
    return 0;
}

void libctx_free(PPUContext& ctx, u32 c)
{
    Lib& l = lib();
    if (!c) return;
    // The resources' cleanups, in the list's order (no resource left an object in its slot). The first: SSL-C's
    // statics, the OBJ names (ciphers, then digests, then the rest and the table), the X509 methods' cleanup, the
    // error states and strings.
    for (u32* rsa : {&l.static_rsa1, &l.static_rsa2}) {
        if (*rsa) {
            PS_LOG_ERROR(kChan, "libssl's cleanup: an RSA object at {:#x} to free (pspocket's never makes one)", *rsa);
            *rsa = 0;
        }
    }
    obj_name_cleanup(ctx, 2);
    obj_name_cleanup(ctx, 1);
    obj_name_cleanup(ctx, -1);
    err_remove_state(ctx);
    err_free_strings(ctx);
    // The random generator's cleanup (its state is static), then the slots' data (none) and the context.
    for (u32 t = 0; t < kTypes; t++) {
        const u32 d = r32(ctx, c + 12 + 8 * t);
        if (d) {
            Free(ctx, d);
            w32(ctx, c + 12 + 8 * t, 0);
        }
    }
    (void)kCtxFlags;
    Free(ctx, c);
}

s32 rand_seed(PPUContext& ctx, u32 buf, u32 size)
{
    // RAND_seed through the generator's method, under the lock; its pool is libssl's static state, not the heap's.
    (void)buf, (void)size;
    crypto_lock(ctx, 9);
    crypto_lock(ctx, 10);
    return 1;
}

void register_lib(Registry& r)
{
    r.add("cellSsl", {
        PS_HLE_FUNC(0xF5313D34, ERR_peek_error),
        PS_HLE_FUNC(0xA5DACF92, ERR_get_error),
        PS_HLE_FUNC(0x44567B8F, ERR_clear_error),
        PS_HLE_FUNC(0x3FC3BBB1, ERR_error_string),
        PS_HLE_FUNC(0x24510989, ERR_func_error_string),
    });
}

} // namespace pspocket::hle::ssl
