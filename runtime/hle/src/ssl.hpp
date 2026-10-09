#pragma once

// cellSsl's internals shared by its files (runtime/hle/src/cellSsl*.cpp, D-530), and what other HLE modules (cellHttp's
// HTTPS, D-524) take from it.
//
// libssl.sprx is RSA's SSL-C, an SSLeay descendant: OpenSSL 0.9's structures (LHASH, STACK, ERR_STATE, BIO, BUF_MEM,
// the ASN.1 objects, X509, SSL_CTX, SSL) over RSA's own crypto library ("R_" functions, a library context of resources).
// It keeps every object in a liblv2 heap made over the pool cellSslInit is given, and its globals in its data segment.
// pspocket does the same: every object is a heap block of libssl's size, made at the same moment, its fields at
// libssl's offsets, so the pool's bytes are libssl's. libssl's globals are host state here, except what the pool points
// at (strings, tables, the functions an object names): those live in a guest block of pspocket's (Lib::block), where a
// function libssl names by descriptor is an address of its own ("function ids", fn()).

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/types.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/ppu/context.h"

namespace pspocket::hle {

// Whether cellSslInit ran and cellSslEnd hasn't since (RPCS3 _cellSslIsInitd): cellHttpsEnd needs it.
bool ssl_initialized();

namespace ssl {

inline constexpr std::string_view kChan = "cellSsl";

// RPCS3 cellSsl.h (CellSslError).
enum : u32 {
    CELL_SSL_ERROR_NOT_INITIALIZED = 0x80740001,
    CELL_SSL_ERROR_ALREADY_INITIALIZED = 0x80740002,
    CELL_SSL_ERROR_INITIALIZATION_FAILED = 0x80740003,
    CELL_SSL_ERROR_NO_BUFFER = 0x80740004,
    CELL_SSL_ERROR_INVALID_CERTIFICATE = 0x80740005,
    CELL_SSL_ERROR_UNRETRIEVABLE = 0x80740006,
    CELL_SSL_ERROR_INVALID_FORMAT = 0x80740007,
    CELL_SSL_ERROR_NOT_FOUND = 0x80740008,
    CELL_SSL_ERROR_INVALID_TIME = 0x80740031,
    CELL_SSL_ERROR_INAVLID_NEGATIVE_TIME = 0x80740032,
    CELL_SSL_ERROR_INCORRECT_TIME = 0x80740033,
    CELL_SSL_ERROR_UNDEFINED_TIME_TYPE = 0x80740034,
    CELL_SSL_ERROR_NO_MEMORY = 0x80740035,
    CELL_SSL_ERROR_NO_STRING = 0x80740036,
    CELL_SSL_ERROR_UNKNOWN_LOAD_CERT = 0x80740037,
};
constexpr s32 err(u32 code) { return static_cast<s32>(code); }

// The guest block holding what libssl's pool points at (0 before the first use), and its size.
inline constexpr u32 kGlobalsSize = 0x8000;
u32 globals_block();

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
void copy(PPUContext& ctx, u32 dst, u32 src, u32 size); // memmove
void put(PPUContext& ctx, u32 dst, const void* src, u32 size);
std::vector<u8> bytes(PPUContext& ctx, u32 a, u32 size);
u32 str_len(PPUContext& ctx, u32 s);
std::string str(PPUContext& ctx, u32 s);
s32 str_cmp(PPUContext& ctx, u32 a, u32 b); // liblv2's _sys_strcmp
s32 mem_cmp(PPUContext& ctx, u32 a, u32 b, u32 n);

// A pointer the caller gave, checked before use (D-042): where libssl would fault on memory that is not there, the
// call site is logged and the caller answers with an error of its own.
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what);

// ------------------------------------------------------------------------------------------------- other libraries

// The HLE function libssl imports (module, name), called as libssl calls it.
u64 call(PPUContext& ctx, std::string_view module, std::string_view name, std::initializer_list<u64> args);
inline s32 sc(PPUContext& ctx, u32 number, std::initializer_list<u64> args)
{
    return static_cast<s32>(lv2_syscall(ctx, number, args));
}
// libssl's own check of an address it is given (sys_memory_get_page_attribute, 351): whether it is mapped.
bool page_ok(PPUContext& ctx, u32 addr);

// ------------------------------------------------------------------------------------------------- the library's state

// Functions and tables libssl names by address in its objects: their ids (addresses in the block).
enum class Fn : u32 {
    ObjNameHash,
    ObjNameCmp,
    ErrStateHash,
    ErrStateCmp,
    LibMethods,
    ResourceList,
    Count
};
u32 fn(PPUContext& ctx, Fn f);

// A string of libssl's in the block (the same address for the same text).
u32 lit(PPUContext& ctx, std::string_view text);
// A zeroed object of libssl's data in the block, known by name (an EVP_MD, an EVP_CIPHER, a method table).
u32 object(PPUContext& ctx, std::string_view name, u32 size);

struct Lib {
    u32 block = 0, block_used = 0;
    std::vector<std::pair<std::string, u32>> literals;
    // cellSsl's globals: the heap over the pool, the allocation failure callback and its argument, the library
    // context, the lightweight mutex "_sslsys" (in the block), initialised.
    u32 heap = 0, mem_cb = 0, mem_cb_arg = 0, libctx = 0, lwmutex = 0;
    bool initialized = false;
    // SSL-C's: memory functions and the lock callback set (cellSslInit sets them; nothing unsets them), the X509
    // methods installed, the time callback set.
    bool mem_funcs = false, lock_cb = false, x509_methods = false, time_cb = false;
    // Hash tables: OBJ_NAME's, the ERR states', the ERR strings' (never filled: libssl loads no error strings).
    u32 obj_names = 0, err_states = 0, err_strings = 0;
    // OBJ_NAME_cleanup's type filter.
    s32 obj_names_cleanup_type = 0;
    // Statics libssl's cleanup frees (SSL-C's RSA objects, made by the first handshake).
    u32 static_rsa1 = 0, static_rsa2 = 0;
};
Lib& lib();

// ------------------------------------------------------------------------------------------------- memory, locks

// libssl's malloc, realloc and free (SSL-C's, over the heap): an allocation that fails calls the game's callback
// (cellSsl_4EE83087) with the heap's state.
u32 Malloc(PPUContext& ctx, u32 size);
u32 Realloc(PPUContext& ctx, u32 p, u32 size, u32 old_size);
void Free(PPUContext& ctx, u32 p);
// CRYPTO_lock(mode, ...): the lock callback cellSslInit sets takes "_sslsys" for a lock (mode & 1), else lets it go.
void crypto_lock(PPUContext& ctx, u32 mode);

// ------------------------------------------------------------------------------------------------- LHASH

// OpenSSL 0.9's LHASH (96 bytes: the buckets +0, comp +4, hash +8, num_nodes +12, num_alloc_nodes +16, p +20, pmax
// +24, up_load +28, down_load +32, num_items +36, then statistics, error +92); nodes of 12 bytes: data, next, hash.
u32 lh_new(PPUContext& ctx, u32 hash_fn, u32 comp_fn);
u32 lh_insert(PPUContext& ctx, u32 lh, u32 data); // the data replaced, or 0
u32 lh_delete(PPUContext& ctx, u32 lh, u32 data);
u32 lh_retrieve(PPUContext& ctx, u32 lh, u32 data);
void lh_free(PPUContext& ctx, u32 lh);
// lh_doall: the buckets from the last to the first, each one's nodes in order.
template <class F>
void lh_doall(PPUContext& ctx, u32 lh, F&& f)
{
    const u32 b = r32(ctx, lh);
    for (s32 i = static_cast<s32>(r32(ctx, lh + 12)) - 1; i >= 0; i--) {
        for (u32 n = r32(ctx, b + 4 * static_cast<u32>(i)); n;) {
            const u32 data = r32(ctx, n), next = r32(ctx, n + 4);
            f(data);
            n = next;
        }
    }
}
u32 lh_strhash(PPUContext& ctx, u32 s);

// ------------------------------------------------------------------------------------------------- objects, names

// The object table (OBJ_nid2sn, OBJ_nid2ln): RSA's object numbers' names, in the block.
u32 obj_nid2sn(PPUContext& ctx, u32 nid);
u32 obj_nid2ln(PPUContext& ctx, u32 nid);
// OBJ_NAME_add(name, type, data): 1, or 0.
s32 obj_name_add(PPUContext& ctx, u32 name, u32 type, u32 data);
void obj_name_cleanup(PPUContext& ctx, s32 type);

// ------------------------------------------------------------------------------------------------- errors

// ERR_put_error(lib, func, reason, file, line): into the calling thread's ERR_STATE (made at its first error).
void put_error(PPUContext& ctx, u32 libno, u32 func, u32 reason, std::string_view file, u32 line);
u32 err_get_state(PPUContext& ctx);
// ERR_get_error (take) and ERR_peek_error.
u32 err_get_error(PPUContext& ctx, bool take);
void err_clear(PPUContext& ctx);
void err_remove_state(PPUContext& ctx);
void err_free_strings(PPUContext& ctx);

// ------------------------------------------------------------------------------------------------- the library context

// R_LIB_CTX_new over libssl's resource list (its OBJ names, digests, ciphers, the X509 methods): 0, or SSL-C's error.
s32 libctx_new(PPUContext& ctx, u32& out);
void libctx_free(PPUContext& ctx, u32 libctx);
// RAND_seed with the console's random bytes: 1, or 0.
s32 rand_seed(PPUContext& ctx, u32 buf, u32 size);

// Registration of each file's functions.
void register_lib(Registry& r);

} // namespace ssl

} // namespace pspocket::hle
