// cellSsl: Sony's libssl.sprx pspocket's own (D-530). Every function does what libssl (4.92) does, read from its code
// and compared call for call with it in tests/hle/test_ssl_compare.cpp: the same answers, the same bytes in the caller's
// memory and in libssl's pool (its heap: the same blocks at the same places, the objects' fields at libssl's offsets),
// the same lv2 calls. RPCS3's rpcs3/Emu/Cell/Modules/cellSsl.cpp/.h gives the names, the error codes and the
// certificate flags (its functions are stubs; RPCS3 runs Sony's module).
//
// This file: Sony's layer over SSL-C (cellSslInit/End, the memory report and callback, the certificate loader, the
// cellSslCert* queries). The other files: SSL-C's common layer (cellSslLib.cpp), BIOs (cellSslBio.cpp), ASN.1 and
// X509 (cellSslX509.cpp), SSL itself (cellSslConn.cpp).

#include <cstring>
#include <format>
#include <string_view>

#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "ssl.hpp"

namespace pspocket::hle {

bool ssl_initialized() { return ssl::lib().initialized; }

namespace ssl {

namespace {

// cellSslInit(pool, size): a heap "cellSsl_Library" over the pool (liblv2's, on the caller's memory), the lock
// "_sslsys" (recursive), SSL-C's memory functions and lock callback, the library context, 64 random bytes from lv2 to
// seed SSL-C's generator, the time callback (cellRtc).
s32 cellSslInit(PPUContext& ctx, u32 pool, u32 pool_size)
{
    Lib& l = lib();
    if (l.initialized) return err(CELL_SSL_ERROR_ALREADY_INITIALIZED);
    if (!pool || !pool_size) return err(CELL_SSL_ERROR_NO_BUFFER);
    const u32 name = heap_name(ctx);
    StackVar attr(ctx, gb(ctx), 16); // the heap's attribute {8, address}
    w32(ctx, attr.addr(), 8);
    w32(ctx, attr.addr() + 4, pool);
    const u32 heap = static_cast<u32>(call(ctx, "sysPrxForUser", "_sys_heap_create_heap",
        {name, pool_size, 0xFFFFFFFF80000000ull, attr.addr()}));
    if (!heap) return err(CELL_SSL_ERROR_NO_MEMORY);
    l.heap = heap;
    l.mem_cb = 0;
    l.mem_cb_arg = 0;
    {
        StackVar lw(ctx, gb(ctx), 16); // sys_lwmutex_attribute_t {protocol, recursive, name[8]}
        w32(ctx, lw.addr(), 2);        // SYS_SYNC_PRIORITY
        w32(ctx, lw.addr() + 4, 0x10); // SYS_SYNC_RECURSIVE
        std::memcpy(gb(ctx) + lw.addr() + 8, "_sslsys", 8);
        call(ctx, "sysPrxForUser", "sys_lwmutex_create", {l.lwmutex, lw.addr()});
    }
    l.mem_funcs = true;
    l.lock_cb = true;
    if (libctx_new(ctx, l.libctx) == 0) {
        StackVar rnd(ctx, gb(ctx), 64);
        if (static_cast<s32>(call(ctx, "sysPrxForUser", "sys_get_random_number", {rnd.addr(), 64})) == 0 &&
            rand_seed(ctx, rnd.addr(), 64)) {
            l.time_cb = true;
            l.initialized = true;
            return lv2::CELL_OK;
        }
        if (l.libctx) {
            libctx_free(ctx, l.libctx);
            l.libctx = 0;
        }
    }
    call(ctx, "sysPrxForUser", "_sys_heap_delete_heap", {heap});
    l.heap = 0;
    call(ctx, "sysPrxForUser", "sys_lwmutex_destroy", {l.lwmutex});
    return err(CELL_SSL_ERROR_INITIALIZATION_FAILED);
}

// cellSslEnd: the library context freed (and with it what SSL-C keeps), the heap deleted, the lock destroyed.
s32 cellSslEnd(PPUContext& ctx)
{
    Lib& l = lib();
    if (!l.initialized) return err(CELL_SSL_ERROR_NOT_INITIALIZED);
    if (l.libctx) {
        libctx_free(ctx, l.libctx);
        l.libctx = 0;
    }
    const u32 heap = l.heap;
    l.mem_cb_arg = 0;
    l.heap = 0;
    l.initialized = false;
    l.mem_cb = 0;
    call(ctx, "sysPrxForUser", "_sys_heap_delete_heap", {heap});
    call(ctx, "sysPrxForUser", "sys_lwmutex_destroy", {l.lwmutex});
    return lv2::CELL_OK;
}

// _cellSslIsInitd.
u32 _cellSslIsInitd(PPUContext&) { return lib().initialized ? 1 : 0; }

// cellSsl_4EE83087(callback, arg): the function an allocation that fails calls (with the heap's free size, the size
// asked for, the heap's mallinfo and `arg`); once initialised.
s32 cellSsl_4EE83087(PPUContext&, u32 callback, u32 arg)
{
    Lib& l = lib();
    if (!l.initialized) return err(CELL_SSL_ERROR_NOT_INITIALIZED);
    l.mem_cb = callback;
    l.mem_cb_arg = arg;
    return lv2::CELL_OK;
}

// cellSsl_E95B6A4C: an empty function in libssl (r3 stays the caller's).
void cellSsl_E95B6A4C(PPUContext&) {}

// cellSslGetMemoryInfo(u32* free, u32* arena, mallinfo* info): the heap's total free size and liblv2's mallinfo (ten
// words); no check that libssl is initialised (liblv2 then answers for heap 0).
s32 cellSslGetMemoryInfo(PPUContext& ctx, u32 free_out, u32 arena_out, u32 info_out)
{
    Lib& l = lib();
    const u32 free_size = static_cast<u32>(call(ctx, "sysPrxForUser", "_sys_heap_get_total_free_size", {l.heap}));
    StackVar mi(ctx, gb(ctx), 40);
    call(ctx, "sysPrxForUser", "_sys_heap_get_mallinfo", {l.heap, mi.addr()});
    if (free_out && usable(ctx, free_out, 4, "cellSslGetMemoryInfo: free")) w32(ctx, free_out, free_size);
    if (arena_out && usable(ctx, arena_out, 4, "cellSslGetMemoryInfo: arena")) w32(ctx, arena_out, r32(ctx, mi.addr()));
    if (info_out && usable(ctx, info_out, 40, "cellSslGetMemoryInfo: info")) copy(ctx, info_out, mi.addr(), 40);
    return lv2::CELL_OK;
}

// cellSslCertificateLoader(u64 flag, char* buffer, u32 size, u32* required): until the loader is libssl's (it reads
// the console's certificates from /dev_flash), RPCS3's answer without firmware.
s32 cellSslCertificateLoader(PPUContext& ctx, u64 flag, u32 buffer, u32 size, u32 required)
{
    (void)flag;
    if (required) {
        if (!usable(ctx, required, 4, "cellSslCertificateLoader: required")) return err(CELL_SSL_ERROR_NO_BUFFER);
        w32(ctx, required, 0);
        return lv2::CELL_OK;
    }
    if (size > 1) {
        if (!usable(ctx, buffer, size - 1, "cellSslCertificateLoader: buffer")) return err(CELL_SSL_ERROR_NO_BUFFER);
        zero(ctx, buffer, size - 1);
    }
    return lv2::CELL_OK;
}

} // namespace

} // namespace ssl

void register_cellSsl(Registry& r)
{
    using namespace ssl;
    r.add("cellSsl", {
        PS_HLE_FUNC(0xFB02C9D2, cellSslInit),
        PS_HLE_FUNC(0x1650AEA4, cellSslEnd),
        PS_HLE_FUNC(0x23F0FD4E, _cellSslIsInitd),
        PS_HLE_FUNC(0x4EE83087, cellSsl_4EE83087),
        PS_HLE_FUNC(0xE95B6A4C, cellSsl_E95B6A4C),
        PS_HLE_FUNC(0x33CB6159, cellSslGetMemoryInfo),
        PS_HLE_FUNC(0x571AFACA, cellSslCertificateLoader),
    });
    register_lib(r);
    register_conn(r);
}

} // namespace pspocket::hle
