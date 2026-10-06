// sys_rsx: lv2's side of the RSX. It allocates RSX local memory and the context regions (DMA control, driver
// information, reports), keeps the IO map (which main memory the RSX can reach), and owns the RSX front end
// (runtime/rsx), whose thread starts when the context is allocated. Reference: RPCS3 rpcs3/Emu/Cell/lv2/sys_rsx.cpp
// and sys_rsx.h (structure layouts), RSXThread.cpp thread::init.
//
// These are called by the HLE libgcm_sys (cellGcmSys), as RPCS3's HLE does. They are not bound to syscall numbers
// 666-677 yet: no game has called them directly (CLAUDE.md rule 4).
//
// Layout: local memory at 0xC0000000. The rsx_context area takes the first free 256 MB slot (as RPCS3's
// vm::reserve_map(vm::rsx_context, ...)); sys_rsx_device_map takes its first 1 MB and sys_rsx_context_allocate the
// next 3 MB: DMA control, driver information at +1 MB, reports at +2 MB. In a typical run the area lands at
// 0x40000000, which is rsx::ContextLayout::rpcs3_default().
//
// Deviations:
// - One context per process (RPCS3 aborts on a second one too); context free and device unmap are not implemented.
// - The driver information's handler event queue and port are not created: the HLE interrupt handlers do not go
//   through lv2 event queues.
// - Display flip (0x102), display queue (0x103), flip status (0x10a), and RPCS3's flip-event (0xFEC) and user-command
//   (0xFEF) hacks are not implemented (the HLE flip goes through the FIFO: GCM_FLIP_COMMAND, and the RSX's on_flip
//   does what 0xFEC does); they log and return CELL_EINVAL until a game needs them. The vblank hack (0xFED) counts
//   vblanks for the HLE GCM interrupt thread.

#include <format>

#include "internal.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/rsx/context.hpp"
#include "pspocket/rsx/null_backend.hpp"
#include "pspocket/rsx/rsx.hpp"

namespace pspocket::lv2 {

namespace {

constexpr std::string_view kRsxChan = "sys_rsx";
constexpr u32 kContextId = 0x55555555;
constexpr u32 kMemHandle = 0x5a5a5a5b;
constexpr u64 kAreaSize = 0x10000000;
constexpr u32 kDeviceSize = 0x100000;
constexpr u32 kContextSize = 0x300000;
// lv1 error (RPCS3 LV1_ILLEGAL_PARAMETER_VALUE = -17).
constexpr Result LV1_ILLEGAL_PARAMETER_VALUE = -17;

// RsxDriverInfo (RPCS3 sys_rsx.h): offsets used here.
namespace di {
constexpr u32 version_driver = 0x0, version_gpu = 0x4, memory_size = 0x8, hardware_channel = 0xC,
              nvcore_frequency = 0x10, memory_frequency = 0x14, reportsNotifyOffset = 0x2C, reportsOffset = 0x30,
              reportsReportOffset = 0x34, systemModeFlags = 0x54, head = 0x10B8, head_size = 0x40, size = 0x12F8;
// RsxDriverInfo::Head
constexpr u32 h_lastFlipTime = 0x0, h_flipFlags = 0x8, h_flipBufferId = 0x10, h_lastVTimeLow = 0x1C,
              h_vBlankCount = 0x30, h_lastVTimeHigh = 0x3C;
} // namespace di

RsxDevice& dev(PpuThread& t) { return t.kernel.impl().rsx; }

} // namespace

RsxDevice::~RsxDevice() { stop(); }

void RsxDevice::stop()
{
    if (rsx) rsx->stop();
}

Result sys_rsx_memory_allocate(PpuThread& t, u32 mem_handle, u32 mem_addr, u64 size, u64 flags, u64 a5, u64 a6, u64 a7)
{
    PS_LOG_INFO(kRsxChan, "sys_rsx_memory_allocate(size {:#x}, flags {:#x}, {:#x}, {:#x}, {:#x})", size, flags, a5, a6,
        a7);
    Kernel::Impl& s = t.kernel.impl();
    if (!size) {
        // RPCS3: "size == 0 yields available size, unimplemented" (an ensure there).
        PS_LOG_ERROR(kRsxChan, "sys_rsx_memory_allocate: size 0 (query the available size) is not implemented");
        return err(CELL_EINVAL);
    }
    if (size & 0xFFFFF) return LV1_ILLEGAL_PARAMETER_VALUE;
    size %= 0x100000ull * 0x100000000ull; // "a result from how the argument is treated internally" (RPCS3)
    if (size > PS_MEM_RSX_LOCAL_SIZE) return LV1_ILLEGAL_PARAMETER_VALUE;
    if (!s.guest.valid(mem_handle, 4) || !s.guest.valid(mem_addr, 8)) return err(CELL_EFAULT);

    RsxDevice& d = s.rsx;
    std::lock_guard lock(d.mutex);
    if (d.local_size) return err(CELL_ENOMEM); // RPCS3 vm::falloc fails on the second allocation
    if (!t.kernel.memory().commit(PS_MEM_RSX_LOCAL_BASE, size)) return err(CELL_ENOMEM);
    d.local_size = static_cast<u32>(size);
    if (d.driver_info) s.guest.w32(d.driver_info + di::memory_size, d.local_size);
    s.guest.w64(mem_addr, PS_MEM_RSX_LOCAL_BASE);
    s.guest.w32(mem_handle, kMemHandle);
    PS_LOG_INFO(kRsxChan, "RSX local memory: {:#010x}, {} MB", PS_MEM_RSX_LOCAL_BASE, size >> 20);
    return CELL_OK;
}

Result sys_rsx_device_map(PpuThread& t, u32 dev_addr, u32 a2, u32 dev_id)
{
    (void)a2; // unused (RPCS3)
    Kernel::Impl& s = t.kernel.impl();
    if (dev_id != 8) {
        // RPCS3 aborts ("Invalid dev_id"); cellGcmInitPerfMon uses 7, 9..12.
        PS_LOG_ERROR(kRsxChan, "sys_rsx_device_map: device {} is not implemented (only 8)", dev_id);
        return err(CELL_EINVAL);
    }
    if (!s.guest.valid(dev_addr, 8)) return err(CELL_EFAULT);

    RsxDevice& d = s.rsx;
    std::lock_guard lock(d.mutex);
    if (!d.device_addr) {
        std::lock_guard mem_lock(s.memory.mutex);
        Area* area = s.memory.reserve_area(Area::Kind::RsxContext, kAreaSize, kAreaSize, 0x100000);
        if (!area) return err(CELL_ENOMEM);
        if (!t.kernel.memory().commit(area->addr, kDeviceSize + kContextSize)) {
            std::erase_if(s.memory.areas, [&](const auto& a) { return a.get() == area; });
            return err(CELL_ENOMEM);
        }
        d.area = area->addr;
        d.device_addr = area->addr;
        PS_LOG_INFO(kRsxChan, "sys_rsx_device_map: RSX device at {:#010x}", d.device_addr);
    }
    s.guest.w64(dev_addr, d.device_addr);
    return CELL_OK;
}

Result sys_rsx_context_allocate(PpuThread& t, u32 context_id, u32 lpar_dma_control, u32 lpar_driver_info,
    u32 lpar_reports, u64 mem_ctx, u64 system_mode)
{
    PS_LOG_INFO(kRsxChan, "sys_rsx_context_allocate(mem_ctx {:#x}, system mode {:#x})", mem_ctx, system_mode);
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(context_id, 4) || !s.guest.valid(lpar_dma_control, 8) || !s.guest.valid(lpar_driver_info, 8) ||
        !s.guest.valid(lpar_reports, 8)) {
        return err(CELL_EFAULT);
    }

    RsxDevice& d = s.rsx;
    std::lock_guard lock(d.mutex);
    if (!d.local_size) return err(CELL_EINVAL); // no local memory yet (RPCS3 checks local_mem_base)
    if (d.dma_control) {
        PS_LOG_ERROR(kRsxChan, "sys_rsx_context_allocate: a second RSX context is not supported (RPCS3 aborts too)");
        return err(CELL_EBUSY);
    }
    if (!d.device_addr) {
        // RPCS3 allocates the context from the area wherever it is; libgcm always maps the device first.
        PS_LOG_ERROR(kRsxChan, "sys_rsx_context_allocate before sys_rsx_device_map is not implemented");
        return err(CELL_ENOMEM);
    }

    rsx::ContextLayout layout;
    layout.device_addr = d.device_addr;
    layout.dma_control_addr = d.device_addr + kDeviceSize;
    layout.driver_info_addr = layout.dma_control_addr + 0x100000;
    layout.reports_addr = layout.dma_control_addr + 0x200000;

    // Reports, DMA control (put = get = 0, ref = 0 until cellGcmSys sets it) and semaphores, as RPCS3.
    if (!rsx::initialize_context_memory(t.kernel.memory(), layout)) return err(CELL_ENOMEM);
    s.guest.w32(layout.ref_addr(), 0);

    const u32 info = layout.driver_info_addr;
    std::memset(s.guest.ptr(info), 0, di::size);
    s.guest.w32(info + di::version_driver, 0x211);
    s.guest.w32(info + di::version_gpu, 0x5c);
    s.guest.w32(info + di::memory_size, d.local_size);
    s.guest.w32(info + di::nvcore_frequency, 500000000);
    s.guest.w32(info + di::memory_frequency, 650000000);
    s.guest.w32(info + di::reportsNotifyOffset, 0x1000);
    s.guest.w32(info + di::reportsOffset, 0);
    s.guest.w32(info + di::reportsReportOffset, 0x1400);
    s.guest.w32(info + di::systemModeFlags, static_cast<u32>(system_mode));
    s.guest.w32(info + di::hardware_channel, 1); // "1 for games, 0 for vsh" (RPCS3)
    // RSXThread.cpp thread::init: the device's flip semaphore.
    s.guest.w32(d.device_addr + 0x30, 1);

    d.dma_control = layout.dma_control_addr;
    d.driver_info = info;
    d.reports = layout.reports_addr;
    // The IO address space: 512 MB in system mode CELL_GCM_SYSTEM_MODE_IOMAP_512MB (cellGcmInitSystemMode(1); RPCS3
    // sets the renderer's main_mem_size in _cellGcmInitBody from the same mode), else 256 MB.
    d.main_mem_size = system_mode == 1 ? 0x20000000 : 0x10000000;

    rsx::Config config;
    config.layout = layout;
    config.local_memory_size = d.local_size;
    if (t.kernel.config().rsx_log_failures) {
        config.unhandled_policy = rsx::FailPolicy::Log;
    }
    const auto& make_backend = t.kernel.config().rsx_backend;
    d.backend = make_backend ? make_backend() : nullptr;
    if (!d.backend) d.backend = std::make_unique<rsx::NullBackend>();
    d.rsx = std::make_unique<rsx::Rsx>(t.kernel.memory(), *d.backend, config);
    d.rsx->on_flip = [&s, info](u32 buffer_id) {
        // RPCS3 sys_rsx_context_attribute 0xFEC (what RSXThread's flip calls): head 1 records the flip.
        const u32 head = info + di::head + 1 * di::head_size;
        s.guest.w32(head + di::h_flipFlags, s.guest.r32(head + di::h_flipFlags) | 0x80000000);
        s.guest.w64(head + di::h_lastFlipTime, rsx::Rsx::timestamp_ns());
        s.guest.w32(head + di::h_flipBufferId, buffer_id);
        RsxDevice& dv = s.rsx;
        dv.flips.fetch_add(1, std::memory_order_relaxed);
        std::function<void(u32)> listener;
        {
            std::lock_guard l(dv.listener_mutex);
            listener = dv.on_flip;
        }
        if (listener) listener(buffer_id);
    };
    d.rsx->on_user_command = [&s](u32 arg) {
        std::function<void(u32)> listener;
        {
            std::lock_guard l(s.rsx.listener_mutex);
            listener = s.rsx.on_user_command;
        }
        if (listener) listener(arg);
        else PS_LOG_WARN(kRsxChan, "GCM_SET_USER_COMMAND {:#x} with no user handler", arg);
    };
    d.rsx->start();

    s.guest.w64(lpar_dma_control, layout.dma_control_addr);
    s.guest.w64(lpar_driver_info, layout.driver_info_addr);
    s.guest.w64(lpar_reports, layout.reports_addr);
    s.guest.w32(context_id, kContextId);
    PS_LOG_INFO(kRsxChan,
        "RSX context: DMA control {:#010x}, driver info {:#010x}, reports {:#010x}; RSX thread started",
        layout.dma_control_addr, layout.driver_info_addr, layout.reports_addr);
    return CELL_OK;
}

Result sys_rsx_context_iomap(PpuThread& t, u32 context_id, u64 io, u64 ea, u64 size, u64 flags)
{
    PS_LOG_DEBUG(kRsxChan, "sys_rsx_context_iomap(io {:#x}, ea {:#x}, size {:#x}, flags {:#x})", io, ea, size, flags);
    RsxDevice& d = dev(t);
    if (!size || (io & 0xFFFFF) || size > 0x20000000 || size > std::min<u64>(~io, ~ea) || (ea & 0xFFFFF) ||
        (size & 0xFFFFF)) {
        return err(CELL_EINVAL);
    }
    std::lock_guard lock(d.mutex);
    if (context_id != kContextId || !d.rsx || d.main_mem_size < io + size) return err(CELL_EINVAL);
    for (u64 a = ea; a < ea + size; a += 0x100000) {
        // RPCS3 checks the pages are readable (and 1 MB pages above 0x20000000) and not sys_vm memory.
        if (!t.kernel.memory().is_committed(static_cast<u32>(a))) return err(CELL_EINVAL);
    }
    if (!d.rsx->io_map().map(static_cast<u32>(io), static_cast<u32>(ea), static_cast<u32>(size))) {
        return err(CELL_EINVAL);
    }
    return CELL_OK;
}

Result sys_rsx_context_iounmap(PpuThread& t, u32 context_id, u64 io, u64 size)
{
    RsxDevice& d = dev(t);
    if (!size || (size & 0xFFFFF) || (io & 0xFFFFF) || size > 0x20000000 || size > ~io) return err(CELL_EINVAL);
    std::lock_guard lock(d.mutex);
    if (context_id != kContextId || !d.rsx || d.main_mem_size < io + size) return err(CELL_EINVAL);
    d.rsx->io_map().unmap(static_cast<u32>(io), static_cast<u32>(size));
    return CELL_OK;
}

Result sys_rsx_context_attribute(PpuThread& t, u32 context_id, u32 package_id, u64 a3, u64 a4, u64 a5, u64 a6)
{
    RsxDevice& d = dev(t);
    std::lock_guard lock(d.mutex);
    if (!d.rsx) return err(CELL_EINVAL);
    if (context_id != kContextId) return err(CELL_EINVAL);

    switch (package_id) {
    case 0x001: // FIFO get and put
        d.rsx->set_fifo_pointers(static_cast<u32>(a3), static_cast<u32>(a4));
        return CELL_OK;
    case 0x100: // display mode
        return CELL_OK;
    case 0x101: // display sync (cellGcmSetFlipMode): 2 = vsync
        d.vsync = a4 == 2;
        return CELL_OK;
    case 0x104: { // display buffer
        const u32 id = a3 & 0xFF;
        if (id > 7) return LV1_ILLEGAL_PARAMETER_VALUE;
        rsx::DisplayBuffer b;
        b.width = static_cast<u32>(a4 >> 32);
        b.height = static_cast<u32>(a4);
        b.pitch = static_cast<u32>(a5 >> 32);
        b.offset = static_cast<u32>(a5);
        d.rsx->set_display_buffer(id, b);
        PS_LOG_INFO(kRsxChan, "display buffer {}: offset {:#x}, {}x{}, pitch {}", id, b.offset, b.width, b.height,
            b.pitch);
        return CELL_OK;
    }
    case 0x105: // destroy buffer? (no-op in RPCS3)
    case 0x106: // cellGcmInitPerfMon
    case 0x10D: // cellGcmInitCursor
    case 0x302: // "something with zcull"
    case 0x600: // framebuffer setup, blit, blit sync, close
    case 0x601:
    case 0x602:
    case 0x603:
        return CELL_OK;
    case 0x300: { // tiles (RPCS3 decodes the packed CellGcmTileInfo words)
        if (a3 >= d.tiles.size()) return LV1_ILLEGAL_PARAMETER_VALUE;
        RsxDevice::Tile& tile = d.tiles[a3];
        const bool bound = ((a4 >> 32) & 0x3) != 0;
        tile.location = static_cast<u32>(((a4 >> 32) & 0x3) - 1);
        tile.offset = static_cast<u32>(((((a4 >> 32) & 0x7FFFFFFF) >> 16) * 0x10000));
        tile.size = static_cast<u32>(((((a4 & 0x7FFFFFFF) >> 16) + 1) * 0x10000) - tile.offset);
        tile.pitch = static_cast<u32>((((a5 >> 32) & 0xFFFFFFFF) >> 8) * 0x100);
        tile.comp = static_cast<u32>(((a5 & 0xFFFFFFFF) >> 26) & 0xF);
        tile.base = static_cast<u32>(a5 & 0x7FF);
        if (bound && (!tile.size || !tile.pitch)) return err(CELL_EINVAL);
        tile.bound = bound;
        PS_LOG_DEBUG(kRsxChan, "tile {}: {} location {}, offset {:#x}, size {:#x}, pitch {:#x}, comp {}", a3,
            bound ? "bound" : "unbound", tile.location, tile.offset, tile.size, tile.pitch, tile.comp);
        return CELL_OK;
    }
    case 0x301: { // Z-cull
        if (a3 >= d.zculls.size()) return LV1_ILLEGAL_PARAMETER_VALUE;
        RsxDevice::Zcull& z = d.zculls[a3];
        z.width = static_cast<u32>(((a4 & 0xFFFFFFFF) >> 22) << 6);
        z.height = static_cast<u32>(((a4 & 0x0000FFFF) >> 6) << 6);
        z.cull_start = static_cast<u32>((a5 >> 32) & ~0xFFFull);
        z.offset = static_cast<u32>(a5 & 0x0FFFFFFF);
        z.bound = (a6 & 0xFFFFFFFF) != 0;
        return CELL_OK;
    }
    case 0xFED: { // vblank (RPCS3's "hack: vblank command", sent by its vblank thread)
        // Deviation: RPCS3 refuses it from a PPU thread; pspocket's vblank source is the HLE GCM interrupt thread.
        if (a3 >= 2) return err(CELL_EINVAL);
        const u32 head = d.driver_info + di::head + static_cast<u32>(a3) * di::head_size;
        Guest& gm = t.kernel.impl().guest;
        const u64 now = rsx::Rsx::timestamp_ns();
        gm.w32(head + di::h_lastVTimeLow, static_cast<u32>(now));
        gm.w32(head + di::h_lastVTimeHigh, static_cast<u32>(now >> 32));
        gm.w64(head + di::h_vBlankCount, gm.r64(head + di::h_vBlankCount) + 1);
        return CELL_OK;
    }
    default:
        PS_LOG_ERROR(kRsxChan, "UNIMPLEMENTED sys_rsx_context_attribute package {:#x} ({:#x}, {:#x}, {:#x}, {:#x}) -> "
                               "CELL_EINVAL",
            package_id, a3, a4, a5, a6);
        return err(CELL_EINVAL);
    }
}

} // namespace pspocket::lv2
