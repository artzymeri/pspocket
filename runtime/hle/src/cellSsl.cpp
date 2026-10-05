// cellSsl: the SSL library's setup and the root certificates in the firmware. Online play is a non-goal (D-024): the
// library only has to come up. Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellSsl.cpp/.h. RPCS3 runs Sony's libssl by
// default (g_prx_list: libssl.sprx is not HLE); its HLE functions are the reference here.
//
// cellSslCertificateLoader: RPCS3 reads each requested certificate from the firmware's /dev_flash/data/cert/CAnn.cer
// and reports a missing file as an empty certificate. There is no firmware here (no /dev_flash, CLAUDE.md rule 1), so
// every certificate is missing, and the answer is RPCS3's without firmware: a required size of 0, or a buffer of zeros.
//
// Differences from RPCS3: pointers are checked before use (D-042): one that is not guest memory is logged with the call
// site and refused with CELL_SSL_ERROR_NO_BUFFER (RPCS3 faults). A buffer of size 0 is left alone (RPCS3 clears 4 GB).

#include <atomic>
#include <bit>
#include <cstring>
#include <format>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "ssl.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellSsl";

enum : u32 {
    CELL_SSL_ERROR_NOT_INITIALIZED = 0x80740001,
    CELL_SSL_ERROR_ALREADY_INITIALIZED = 0x80740002,
    CELL_SSL_ERROR_NO_BUFFER = 0x80740004,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

struct SslState {
    std::atomic<bool> initialized{false};
    std::atomic<bool> certificates_logged{false};
};

SslState& ssl() { return HleProcess::current()->state<SslState>(); }

// Whether all of [addr, addr + size) is committed guest memory (checked per host page); if not, the call site is
// logged (D-042: RPCS3 would fault).
bool usable(PPUContext& ctx, u32 addr, u64 size, std::string_view what)
{
    lv2::PpuThread& t = thread_of(ctx);
    bool ok = u64{addr} + size <= GuestMemory::kGuestSize;
    const u64 page = GuestMemory::host_page_size();
    for (u64 a = addr & ~(page - 1); ok && size && a < u64{addr} + size; a += page)
        ok = t.kernel.memory().is_committed(static_cast<u32>(a));
    if (!ok) log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return ok;
}

// RPCS3: CELL_SSL_ERROR_ALREADY_INITIALIZED when initialised; the pool is not checked (libssl's own memory).
s32 cellSslInit(PPUContext&, u32 pool, u32 pool_size)
{
    const s32 r = ssl().initialized.exchange(true) ? error(CELL_SSL_ERROR_ALREADY_INITIALIZED) : lv2::CELL_OK;
    PS_LOG_INFO(kChan, "cellSslInit(pool {:#x}, size {:#x}): {:#x}", pool, pool_size, static_cast<u32>(r));
    return r;
}

s32 cellSslEnd(PPUContext&)
{
    const s32 r = ssl().initialized.exchange(false) ? lv2::CELL_OK : error(CELL_SSL_ERROR_NOT_INITIALIZED);
    PS_LOG_INFO(kChan, "cellSslEnd(): {:#x}", static_cast<u32>(r));
    return r;
}

// cellSslCertificateLoader(u64 flag, char* buffer, u32 size, u32* required). RPCS3: no initialisation needed; with
// `required` it writes the total size of the certificates the flag bits select and leaves the buffer alone; otherwise
// it clears the first size - 1 bytes of the buffer and copies the certificates in. Every certificate is missing here
// (no firmware): the size is 0 and the buffer only cleared.
s32 cellSslCertificateLoader(PPUContext& ctx, u64 flag, u32 buffer, u32 size, u32 required)
{
    if (!ssl().certificates_logged.exchange(true)) {
        PS_LOG_WARN(kChan, "cellSslCertificateLoader(flag {:#x}): {} certificates asked for, none available (no "
                           "firmware; RPCS3 reports missing ones as empty)",
            flag, std::popcount(flag));
    }
    u8* base = guest_base(ctx);
    if (required) {
        if (!usable(ctx, required, 4, "cellSslCertificateLoader: required")) return error(CELL_SSL_ERROR_NO_BUFFER);
        ps_write32(base, required, 0);
        return lv2::CELL_OK;
    }
    if (size > 1) {
        if (!usable(ctx, buffer, size - 1, "cellSslCertificateLoader: buffer")) return error(CELL_SSL_ERROR_NO_BUFFER);
        std::memset(base + buffer, 0, size - 1);
    }
    return lv2::CELL_OK;
}

} // namespace

bool ssl_initialized() { return ssl().initialized.load(); }

void register_cellSsl(Registry& r)
{
    r.add("cellSsl", {
        PS_HLE_FUNC(0xFB02C9D2, cellSslInit),
        PS_HLE_FUNC(0x1650AEA4, cellSslEnd),
        PS_HLE_FUNC(0x571AFACA, cellSslCertificateLoader),
    });
}

} // namespace pspocket::hle
