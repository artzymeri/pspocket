// sysPrxForUser: the liblv2 user-side library (thread and TLS setup, lightweight sync wrappers, printf, ...).
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/sysPrxForUser.cpp and sys_ppu_thread_.cpp.
//
// Implemented so far: sys_initialize_tls. Everything else stays unimplemented until a runtime log shows the game
// calls it (CLAUDE.md rule 4).

#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sysPrxForUser";

// RPCS3 sys_ppu_thread_.cpp sys_initialize_tls. The loader calls it before the executable's entry with the
// executable's PT_TLS segment (RPCS3 ppu_load_exec queues the same HLE call). Deviation: RPCS3 also creates the
// "atexit!" lwmutex and the "_lv2ppu" mutex here; they back functions that are not implemented yet.
void sys_initialize_tls(PPUContext& ctx, u64 main_thread_id, u32 tls_seg_addr, u32 tls_seg_size, u32 tls_mem_size)
{
    PS_LOG_INFO(kChan, "sys_initialize_tls(thread {:#x}, image {:#010x}, size {:#x}, mem_size {:#x})", main_thread_id,
        tls_seg_addr, tls_seg_size, tls_mem_size);
    if (ctx.gpr[13] != 0) return; // already set up

    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "sys_initialize_tls without an HLE process");
    if (tls_seg_size > tls_mem_size) {
        PS_LOG_ERROR(kChan, "sys_initialize_tls: image size {:#x} > memory size {:#x}", tls_seg_size, tls_mem_size);
        return;
    }
    if (!p->initialize_tls({tls_seg_addr, tls_seg_size, tls_mem_size})) {
        PS_LOG_ERROR(kChan, "sys_initialize_tls: TLS already initialized or out of memory");
        return;
    }
    const u32 block = p->alloc_tls();
    if (!block) PS_FATAL(kChan, "sys_initialize_tls: no memory for the main thread's TLS");
    ctx.gpr[13] = u64{block} + kTlsR13Offset;
}

} // namespace

void register_sysPrxForUser(Registry& r)
{
    r.add("sysPrxForUser", {
        PS_HLE_FUNC(0x744680A2, sys_initialize_tls),
    });
}

} // namespace pspocket::hle
