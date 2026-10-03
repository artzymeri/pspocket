#include "pspocket/rsx/context.hpp"

#include "pspocket/core/log.hpp"

namespace pspocket::rsx {

bool initialize_context_memory(GuestMemory& mem, const ContextLayout& layout)
{
    // Semaphore/report defaults: RPCS3 sys_rsx.cpp sys_rsx_context_allocate.
    if (!mem.commit(layout.device_addr, 0x100000) || !mem.commit(layout.dma_control_addr, 0x1000) ||
        !mem.commit(layout.driver_info_addr, 0x2000) || !mem.commit(layout.reports_addr, reports::kSize)) {
        PS_LOG_ERROR("rsx", "committing the RSX context regions failed");
        return false;
    }

    u8* base = mem.base();
    ps_write32(base, layout.put_addr(), 0);
    ps_write32(base, layout.get_addr(), 0);
    ps_write32(base, layout.ref_addr(), 0xFFFFFFFFu);

    static constexpr u32 kSemaphoreInit[4] = {0x1337C0D3, 0x1337BABE, 0x1337BEEF, 0x1337F001};
    for (u32 i = 0; i < reports::kSemaphoreCount; i++) {
        ps_write32(base, layout.reports_addr + i * 4, kSemaphoreInit[i % 4]);
    }
    for (u32 i = 0; i < reports::kNotifyCount; i++) {
        ps_write64(base, layout.notify_addr(i), ~0ull);
        ps_write64(base, layout.notify_addr(i) + 8, 0);
    }
    for (u32 i = 0; i < reports::kReportCount; i++) {
        ps_write64(base, layout.report_addr(i), ~0ull);
        ps_write32(base, layout.report_addr(i) + 8, 0);
        ps_write32(base, layout.report_addr(i) + 12, 0xFFFFFFFFu);
    }
    return true;
}

} // namespace pspocket::rsx
