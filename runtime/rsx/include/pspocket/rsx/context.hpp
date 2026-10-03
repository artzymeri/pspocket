#pragma once

// Where the RSX context lives in guest memory, and the libgcm-visible control registers.
//
// lv2 owns the placement: sys_rsx_device_map and sys_rsx_context_allocate carve these regions out of a 256 MB
// "rsx_context" area (RPCS3 rpcs3/Emu/Cell/lv2/sys_rsx.cpp, Memory/vm.cpp reserve_map). In a typical RPCS3 run the
// area lands at 0x40000000; device_map takes the first 1 MB and context_allocate the next 3 MB, which is what
// ContextLayout::rpcs3_default() returns. lv2 passes the real addresses when it lands.
//
// Control registers (RPCS3 sys_rsx.h RsxDmaControl, cellGcmSys.cpp _cellGcmInitBody):
//   dma_control + 0x40  put   (big-endian u32, written by the guest: FIFO offset of the end of valid commands)
//   dma_control + 0x44  get   (written by the RSX: FIFO offset of the next command it will read)
//   dma_control + 0x48  ref   (written by the RSX on NV406E_SET_REFERENCE)
// cellGcmGetControlRegister() returns dma_control + 0x40 (CellGcmControl {put, get, ref}).
//
// Reports area (RsxReports): 1024 u32 semaphores (labels are every 16 bytes: cellGcmGetLabelAddress(i) =
// reports + 0x10 * i), then 64 notify entries at +0x1000, then 2048 report entries at +0x1400.

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/types.hpp"
#include "pspocket/rsx/gcm.hpp"

namespace pspocket::rsx {

struct ContextLayout {
    u32 device_addr = 0;      // sys_rsx_device_map: 1 MB (DMA_DEVICE_R/RW context, flip semaphore at +0x30)
    u32 dma_control_addr = 0; // sys_rsx_context_allocate lpar_dma_control
    u32 driver_info_addr = 0; // lpar_driver_info (RsxDriverInfo, owned by lv2/HLE)
    u32 reports_addr = 0;     // lpar_reports (labels, notifies, reports)

    static constexpr ContextLayout rpcs3_default()
    {
        return {0x40000000, 0x40100000, 0x40200000, 0x40300000};
    }

    u32 put_addr() const { return dma_control_addr + 0x40; }
    u32 get_addr() const { return dma_control_addr + 0x44; }
    u32 ref_addr() const { return dma_control_addr + 0x48; }
    u32 label_addr(u32 index) const { return reports_addr + reports::kLabelStride * index; }
    u32 notify_addr(u32 index) const { return reports_addr + reports::kNotifyOffset + 16 * index; }
    u32 report_addr(u32 index) const { return reports_addr + reports::kReportOffset + 16 * index; }
};

// Commits the context regions and fills them the way lv2's sys_rsx_context_allocate does: put = get = 0,
// ref = 0xFFFFFFFF (set by cellGcmInit), semaphores = 0x1337C0D3/0x1337BABE/0x1337BEEF/0x1337F001 repeating,
// notify timestamps = ~0, reports {timestamp ~0, value 0, pad ~0}. lv2 may do this itself; the RSX tests use it.
bool initialize_context_memory(GuestMemory& mem, const ContextLayout& layout);

} // namespace pspocket::rsx
