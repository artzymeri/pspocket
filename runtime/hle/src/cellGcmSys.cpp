// cellGcmSys (libgcm_sys): RSX setup for the game's command buffer library. The game links libgcm's command-writing
// part statically and calls these to set up the RSX, map main memory for it and register display buffers; it then
// writes commands into its command buffer and advances the put register itself. Everything here sits on lv2's
// sys_rsx (runtime/lv2/src/rsx.cpp), as in RPCS3, whose RSX front end runs the commands.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellGcmSys.cpp, Emu/RSX/GCM.h (structure layouts and packing).
//
// Guest structures (big-endian):
//   CellGcmContextData {u32 begin, end, current, callback}: the game's command buffer pointers. It lives at the start
//     of the RSX device area, as RPCS3 places it; callback is cellGcmCallback's descriptor.
//   CellGcmControl {u32 put, get, ref} = DMA control + 0x40.
//   CellGcmConfig {localAddress, ioAddress, localSize, ioSize, memoryFrequency, coreFrequency}.
//   Offset table: u16 ioAddress[3072] (EA MB -> IO MB) then u16 eaAddress[512] (IO MB -> EA MB), 0xFFFF = unmapped.
//
// The interrupt thread (_gcm_intr_thread, RPCS3 creates it in _cellGcmInitBody) is an lv2 PPU thread whose entry is
// an internal HLE function: every 1/59.94 s it counts a vblank in the driver information (lv2 package 0xFED, as
// RPCS3's vblank thread does) and calls the game's vblank handler with head 1. RPCS3 runs its handlers on that thread
// too. Flip and user handlers: not imported by any game so far. Tile and Z-cull settings reach lv2, but the RSX front
// end does not use them yet.

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/rsx/gcm.hpp"
#include "pspocket/rsx/rsx.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellGcmSys";
constexpr std::string_view kModule = "cellGcmSys";

enum : u32 {
    CELL_GCM_ERROR_FAILURE = 0x802100ff,
    CELL_GCM_ERROR_NO_IO_PAGE_TABLE = 0x80210001,
    CELL_GCM_ERROR_INVALID_ENUM = 0x80210002,
    CELL_GCM_ERROR_INVALID_VALUE = 0x80210003,
    CELL_GCM_ERROR_INVALID_ALIGNMENT = 0x80210004,
    CELL_GCM_ERROR_ADDRESS_OVERWRAP = 0x80210005,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 CELL_GCM_LOCATION_MAIN = 1; // anything else is local memory (CELL_GCM_LOCATION_LOCAL = 0)
constexpr u32 CELL_GCM_IOMAP_FLAG_STRICT_ORDERING = 1 << 1;
constexpr u32 kContextId = 0x55555555;
constexpr u32 kLocalSize = 0x0F900000; // 249 MB: RPCS3's fixed value (its TODO: by SDK version, gcmGetLocalMemorySize)
constexpr u32 kTilesCount = 15, kZcullsCount = 8;
constexpr u32 kFragment = 32 * 1024; // the default command buffer's 32 KB segments
constexpr u32 NID_cellGcmCallback = 0x8DD39C9C;
constexpr u32 NID_interrupt_thread = 0x24A45AFC; // nid_of("_pspocket_gcm_interrupt_thread"): pspocket's own
constexpr auto kVblankPeriod = std::chrono::microseconds(16683); // 59.94 Hz

// CellGcmContextData
constexpr u32 ctx_begin = 0x0, ctx_end = 0x4, ctx_current = 0x8, ctx_callback = 0xC;
// CellGcmReportData {u64 timer; u32 value; u32 pad}
constexpr u32 kReportsReportOffset = 0x1400;

constexpr std::array<u32, 33> kTiledPitches = {0x00000000, 0x00000200, 0x00000300, 0x00000400, 0x00000500, 0x00000600,
    0x00000700, 0x00000800, 0x00000A00, 0x00000C00, 0x00000D00, 0x00000E00, 0x00001000, 0x00001400, 0x00001800,
    0x00001A00, 0x00001C00, 0x00002000, 0x00002800, 0x00003000, 0x00003400, 0x00003800, 0x00004000, 0x00005000,
    0x00006000, 0x00006800, 0x00007000, 0x00008000, 0x0000A000, 0x0000C000, 0x0000D000, 0x0000E000, 0x00010000};

struct GcmState {
    std::mutex mutex; // RPCS3 gcm_config::gcmio_mutex
    bool initialized = false;
    // CellGcmConfig
    u32 local_addr = 0, local_size = 0, io_address = 0, io_size = 0;
    u32 context_addr = 0; // CellGcmContextData
    u32 control_addr = 0; // CellGcmControl
    u32 label_addr = 0;   // RSX reports area: labels, notifies, reports
    u32 offset_table = 0; // u16 ioAddress[3072], then u16 eaAddress[512]
    u32 display_info = 0; // CellGcmDisplayInfo[8]
    u32 zculls = 0;       // CellGcmZcullInfo[8]
    u32 tiles = 0;        // CellGcmTileInfo[15]
    u32 cmd_begin = 0;    // the default command buffer (RPCS3 g_defaultCommandBufferBegin)
    u32 cmd_fragments = 0;
    u32 main_mem_size = 0x10000000;
    std::atomic<u32> vblank_handler{0};
    std::atomic<u64> vblanks{0};
    struct Tile {
        u32 location = 0, offset = 0, size = 0, pitch = 0, comp = 0, base = 0, bank = 0;
    };
    std::array<Tile, kTilesCount> tile{};
};

GcmState& gcm() { return HleProcess::current()->state<GcmState>(); }

// The IO page of a main-memory MB (0xFFFF: not mapped). The table covers EAs below RSX local memory (3072 MB).
u32 io_address_entry(u8* base, const GcmState& g, u32 ea_mb)
{
    return ea_mb < 3072 ? ps_read16(base, g.offset_table + 2 * ea_mb) : 0xFFFF;
}

// RPCS3 getOffsetFromAddress: the IO offset of a main-memory address through the offset table.
std::optional<u32> io_offset_of(u8* base, const GcmState& g, u32 addr)
{
    const u32 upper = io_address_entry(base, g, addr >> 20);
    if (upper == 0xFFFF) return std::nullopt;
    return (upper << 20) | (addr & 0xFFFFF);
}

// RPCS3 gcmIoOffsetToAddress: 0 when the IO offset is not mapped.
u32 io_offset_to_address(PPUContext& ctx, u32 io)
{
    rsx::Rsx* rsx = thread_of(ctx).kernel.rsx();
    if (io >= 0x20000000 || !rsx) return 0;
    const auto ea = rsx->io_map().io_to_ea(io);
    if (!ea || *ea >= PS_MEM_RSX_LOCAL_BASE) return 0;
    return *ea;
}

// RPCS3 gcmMapEaIoAddress (the caller holds the GCM lock).
s32 map_ea_io(PPUContext& ctx, GcmState& g, u32 ea, u32 io, u32 size, bool strict)
{
    if (!size || (ea & 0xFFFFF) || (io & 0xFFFFF) || (size & 0xFFFFF)) return error(CELL_GCM_ERROR_FAILURE);
    lv2::PpuThread& t = thread_of(ctx);
    if (const s32 r = lv2::sys_rsx_context_iomap(t, kContextId, io, ea, size,
            0xe000000000000800ull | (u64{strict} << 60));
        r != lv2::CELL_OK) {
        return r;
    }
    u8* base = t.kernel.base();
    for (u32 i = 0; i < (size >> 20); i++) {
        ps_write16(base, g.offset_table + 2 * ((ea >> 20) + i), static_cast<u16>((io >> 20) + i));
        ps_write16(base, g.offset_table + 2 * (3072 + (io >> 20) + i), static_cast<u16>((ea >> 20) + i));
    }
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- initialization

// The internal function libgcm calls through CellGcmContextData::callback when the command buffer is full (RPCS3
// cellGcmCallback): flush (put = current), jump to the next 32 KB segment of the default command buffer, and wait
// until the RSX has left that segment.
s32 cellGcmCallback(PPUContext& ctx, u32 context, u32 count)
{
    (void)count;
    GcmState& g = gcm();
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 current = ps_read32(base, context + ctx_current);
    const auto put = io_offset_of(base, g, current);
    if (!put) PS_FATAL(kChan, "cellGcmCallback: command buffer address {:#010x} is not IO mapped", current);
    atomic_store_be32(base, g.control_addr, *put);

    // RPCS3 getNextCommandBufferBeginEnd: segments of 32 KB; the first starts 4 KB in and the last wraps to it.
    const u32 range = (current - g.cmd_begin) / kFragment;
    u32 begin, end;
    if (range + 1 >= g.cmd_fragments) {
        begin = g.cmd_begin + 4096;
        end = g.cmd_begin + kFragment - 4;
    } else {
        begin = g.cmd_begin + (range + 1) * kFragment;
        end = g.cmd_begin + (range + 2) * kFragment - 4;
    }
    const u32 begin_io = *io_offset_of(base, g, begin);
    const u32 end_io = *io_offset_of(base, g, end);
    ps_write32(base, current, rsx::cmd::old_jump(begin_io));
    ps_write32(base, context + ctx_begin, begin);
    ps_write32(base, context + ctx_current, begin);
    ps_write32(base, context + ctx_end, end);
    if (rsx::Rsx* rsx = t.kernel.rsx()) rsx->notify();

    // Wait for the RSX to leave the new segment (RPCS3 isInCommandBufferExcept).
    const auto start = std::chrono::steady_clock::now();
    bool warned = false;
    for (;;) {
        const u32 get = atomic_load_be32(base, g.control_addr + 4);
        if (get < begin_io || get > end_io) break;
        if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            PS_LOG_WARN(kChan, "cellGcmCallback: still waiting for the RSX to leave segment {:#x}..{:#x} (get {:#x})",
                begin_io, end_io, get);
            warned = true;
        }
        std::this_thread::yield();
    }
    return lv2::CELL_OK;
}

// The body of _gcm_intr_thread (see the top of the file). Never returns: the thread unwinds from its sleep when the
// process ends.
void _pspocket_gcm_interrupt_thread(PPUContext& ctx, u64)
{
    GcmState& g = gcm();
    lv2::PpuThread& t = thread_of(ctx);
    auto next = std::chrono::steady_clock::now();
    bool logged = false;
    for (;;) {
        next += kVblankPeriod;
        const auto now = std::chrono::steady_clock::now();
        if (next > now) {
            lv2::sys_timer_usleep(t, static_cast<u64>(
                std::chrono::duration_cast<std::chrono::microseconds>(next - now).count()));
        } else if (now - next > std::chrono::milliseconds(100)) {
            next = now; // fell far behind (a debugger, a stalled host): no burst of vblanks
        }
        lv2::sys_rsx_context_attribute(t, kContextId, 0xFED, 1, 0, 0, 0);
        g.vblanks.fetch_add(1, std::memory_order_relaxed);
        if (const u32 handler = g.vblank_handler.load(std::memory_order_acquire)) {
            if (!logged) {
                PS_LOG_INFO(kChan, "first call of the vblank handler {:#x}", handler);
                logged = true;
            }
            call_guest(ctx, handler, {1});
        }
    }
}

// Creates and starts _gcm_intr_thread (RPCS3: priority 1, 16 KB stack; ours is an ordinary thread, not an lv2
// interrupt thread, since nothing establishes it).
s32 start_interrupt_thread(PPUContext& ctx, HleProcess& p)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 entry = p.descriptor_of(kModule, NID_interrupt_thread);
    if (!entry) PS_FATAL(kChan, "the GCM interrupt thread function was not linked at load");
    const u32 tls = p.alloc_tls();
    if (!tls) return lv2::err(lv2::CELL_ENOMEM);
    StackVar v(ctx, base, 0x30); // thread ID (u64), ppu_thread_param_t {entry, tls}, the name
    ps_write32(base, v.addr() + 8, entry);
    ps_write32(base, v.addr() + 12, tls + kTlsR13Offset);
    std::memcpy(base + v.addr() + 0x10, "_gcm_intr_thread", 17);
    if (const s32 r = lv2::_sys_ppu_thread_create(t, v.addr(), v.addr() + 8, 0, 0, 1, 0x4000, 0, v.addr() + 0x10);
        r != lv2::CELL_OK) {
        p.free_tls(tls);
        return r;
    }
    return lv2::sys_ppu_thread_start(t, static_cast<u32>(ps_read64(base, v.addr())));
}

// _cellGcmInitBody(context**, cmdSize, ioSize, ioAddress): RPCS3 _cellGcmInitBody.
s32 _cellGcmInitBody(PPUContext& ctx, u32 context, u32 cmd_size, u32 io_size, u32 io_address)
{
    PS_LOG_INFO(kChan, "_cellGcmInitBody(context {:#x}, cmdSize {:#x}, ioSize {:#x}, ioAddress {:#x})", context,
        cmd_size, io_size, io_address);
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, context, 4)) return error(CELL_GCM_ERROR_FAILURE);
    if (g.initialized) {
        PS_LOG_ERROR(kChan, "_cellGcmInitBody: a second initialization is not implemented");
        return error(CELL_GCM_ERROR_FAILURE);
    }

    HleProcess& p = *HleProcess::current();
    const u32 callback = p.descriptor_of(kModule, NID_cellGcmCallback);
    if (!callback) PS_FATAL(kChan, "_cellGcmInitBody: cellGcmCallback was not linked at load");

    // Local memory, the device and the context (RPCS3: vm::falloc of local memory, then sys_rsx_device_map and
    // sys_rsx_context_allocate).
    StackVar out(ctx, base, 0x40);
    const u32 v = out.addr();
    if (const s32 r = lv2::sys_rsx_memory_allocate(t, v, v + 8, kLocalSize, 0, 0, 0, 0); r != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "_cellGcmInitBody: RSX local memory: {}", lv2::error_name(r));
        return error(CELL_GCM_ERROR_FAILURE);
    }
    if (const s32 r = lv2::sys_rsx_device_map(t, v + 0x10, 0, 8); r != lv2::CELL_OK) return r;
    const u32 device = static_cast<u32>(ps_read64(base, v + 0x10));
    if (const s32 r = lv2::sys_rsx_context_allocate(t, v + 0x18, v + 0x20, v + 0x28, v + 0x30, 0, 0);
        r != lv2::CELL_OK) {
        return r;
    }
    const u32 dma_control = static_cast<u32>(ps_read64(base, v + 0x20));
    const u32 reports = static_cast<u32>(ps_read64(base, v + 0x30));

    // InitOffsetTable, and the arrays cellGcmSys keeps for the game (RPCS3 vm::alloc in vm::main).
    MainMemory& mm = p.main_memory();
    g.offset_table = mm.alloc((3072 + 512) * 2);
    g.display_info = mm.alloc(16 * 8);
    g.zculls = mm.alloc(24 * kZcullsCount);
    g.tiles = mm.alloc(16 * kTilesCount);
    if (!g.offset_table || !g.display_info || !g.zculls || !g.tiles) return error(CELL_GCM_ERROR_FAILURE);
    std::memset(base + g.offset_table, 0xFF, (3072 + 512) * 2);

    g.local_addr = PS_MEM_RSX_LOCAL_BASE;
    g.local_size = kLocalSize;
    if (const s32 r = map_ea_io(ctx, g, io_address, 0, io_size, false); r != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "_cellGcmInitBody: mapping {:#x} bytes at {:#010x} for the RSX failed: {}", io_size,
            io_address, lv2::error_name(r));
        return error(CELL_GCM_ERROR_FAILURE);
    }
    g.io_address = io_address;
    g.io_size = io_size;
    g.cmd_begin = io_address;
    g.cmd_fragments = cmd_size / kFragment;
    g.label_addr = reports;
    g.control_addr = dma_control + 0x40; // CellGcmControl inside RsxDmaControl

    // The game's context: 4 KB reserved at the start of the command buffer, 4 bytes at the end of a segment for the
    // jump to the next one.
    g.context_addr = device;
    ps_write32(base, device + ctx_begin, io_address + 4096);
    ps_write32(base, device + ctx_end, io_address + kFragment - 4);
    ps_write32(base, device + ctx_current, io_address + 4096);
    ps_write32(base, device + ctx_callback, callback);
    ps_write32(base, context, device);
    g.initialized = true;

    PS_LOG_INFO(kChan, "GCM ready: local memory {:#010x} ({} MB), command buffer {:#010x} ({} segments), IO {} MB, "
                       "context {:#010x}, control {:#010x}, labels {:#010x}",
        g.local_addr, g.local_size >> 20, io_address, g.cmd_fragments, io_size >> 20, device, g.control_addr, reports);
    if (const s32 r = start_interrupt_thread(ctx, p); r != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "_cellGcmInitBody: the interrupt thread could not be created: {}", lv2::error_name(r));
        return error(CELL_GCM_ERROR_FAILURE);
    }
    return lv2::CELL_OK;
}

// RPCS3: a TODO that does nothing.
void _cellGcmFunc15(PPUContext&, u32 context)
{
    PS_LOG_WARN(kChan, "_cellGcmFunc15(context {:#x}): does nothing (as RPCS3, whose purpose is unknown)", context);
}

void cellGcmGetConfiguration(PPUContext& ctx, u32 config)
{
    GcmState& g = gcm();
    u8* base = thread_of(ctx).kernel.base();
    ps_write32(base, config + 0x00, g.local_addr);
    ps_write32(base, config + 0x04, g.io_address);
    ps_write32(base, config + 0x08, g.local_size);
    ps_write32(base, config + 0x0C, g.io_size);
    ps_write32(base, config + 0x10, 650000000); // memoryFrequency
    ps_write32(base, config + 0x14, 500000000); // coreFrequency
}

u32 cellGcmGetControlRegister(PPUContext&) { return gcm().control_addr; }

u32 cellGcmGetLabelAddress(PPUContext&, u8 index) { return gcm().label_addr + 0x10 * index; }

// ------------------------------------------------------------------------------------------------- reports

u32 report_address(PPUContext& ctx, u32 index, u32 location)
{
    if (location == CELL_GCM_LOCATION_MAIN) {
        if (index >= 1024 * 1024) PS_LOG_ERROR(kChan, "report index {} out of range (main)", index);
        const u32 io = 0x0e000000 + index * 0x10;
        const u32 ea = io_offset_to_address(ctx, io);
        if (!ea) PS_LOG_ERROR(kChan, "report {} in main memory: IO offset {:#x} is not mapped", index, io);
        return ea | (io & 0xFFFFF);
    }
    if (index >= 2048) PS_LOG_ERROR(kChan, "report index {} out of range (local)", index);
    return gcm().label_addr + kReportsReportOffset + index * 0x10;
}

u32 cellGcmGetReportDataAddressLocation(PPUContext& ctx, u32 index, u32 location)
{
    return report_address(ctx, index, location);
}

u32 cellGcmGetReportDataLocation(PPUContext& ctx, u32 index, u32 location)
{
    return ps_read32(thread_of(ctx).kernel.base(), report_address(ctx, index, location) + 8);
}

u64 cellGcmGetTimeStampLocation(PPUContext& ctx, u32 index, u32 location)
{
    return ps_read64(thread_of(ctx).kernel.base(), report_address(ctx, index, location));
}

// ------------------------------------------------------------------------------------------------- memory mapping

s32 cellGcmAddressToOffset(PPUContext& ctx, u32 address, u32 offset)
{
    GcmState& g = gcm();
    u8* base = thread_of(ctx).kernel.base();
    u32 result;
    if (address - g.local_addr < g.local_size) {
        result = address - g.local_addr;
    } else {
        const u32 upper = io_address_entry(base, g, address >> 20);
        if ((upper << 20) >= g.main_mem_size) return error(CELL_GCM_ERROR_FAILURE); // 0xFFFF: not mapped
        result = (upper << 20) | (address & 0xFFFFF);
    }
    ps_write32(base, offset, result);
    return lv2::CELL_OK;
}

s32 cellGcmIoOffsetToAddress(PPUContext& ctx, u32 io_offset, u32 address)
{
    const u32 ea = io_offset_to_address(ctx, io_offset);
    if (!ea) {
        PS_LOG_ERROR(kChan, "cellGcmIoOffsetToAddress: IO offset {:#x} is not mapped", io_offset);
        return error(CELL_GCM_ERROR_FAILURE);
    }
    ps_write32(thread_of(ctx).kernel.base(), address, ea | (io_offset & 0xFFFFF));
    return lv2::CELL_OK;
}

s32 cellGcmMapEaIoAddressWithFlags(PPUContext& ctx, u32 ea, u32 io, u32 size, u32 flags)
{
    PS_LOG_INFO(kChan, "cellGcmMapEaIoAddressWithFlags(ea {:#x}, io {:#x}, size {:#x}, flags {:#x})", ea, io, size,
        flags);
    if (flags != CELL_GCM_IOMAP_FLAG_STRICT_ORDERING) {
        // RPCS3: ensure(flags == CELL_GCM_IOMAP_FLAG_STRICT_ORDERING)
        PS_LOG_ERROR(kChan, "cellGcmMapEaIoAddressWithFlags: flags {:#x} other than strict ordering", flags);
        return error(CELL_GCM_ERROR_FAILURE);
    }
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    return map_ea_io(ctx, g, ea, io, size, true);
}

// ------------------------------------------------------------------------------------------------- display and flip

s32 cellGcmSetDisplayBuffer(PPUContext& ctx, u8 id, u32 offset, u32 pitch, u32 width, u32 height)
{
    if (id > 7) return error(CELL_GCM_ERROR_FAILURE);
    GcmState& g = gcm();
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    ps_write32(base, g.display_info + 16 * id + 0x0, offset);
    ps_write32(base, g.display_info + 16 * id + 0x4, pitch);
    ps_write32(base, g.display_info + 16 * id + 0x8, width);
    ps_write32(base, g.display_info + 16 * id + 0xC, height);
    // RPCS3's HLE writes the renderer's display buffers directly; the firmware does it through lv2 (0x104).
    return lv2::sys_rsx_context_attribute(t, kContextId, 0x104, id, (u64{width} << 32) | height,
        (u64{pitch} << 32) | offset, 0);
}

// RPCS3 gcmSetPrepareFlip<old_api = true>: the flip command goes into the buffer without a space check or a put
// update (the game flushes).
void prepare_flip(u8* base, u32 context, u32 id)
{
    const u32 current = ps_read32(base, context + ctx_current);
    ps_write32(base, current, rsx::cmd::method_header(rsx::m::GCM_FLIP_COMMAND, 1));
    ps_write32(base, current + 4, id);
    ps_write32(base, context + ctx_current, current + 8);
}

void _cellGcmSetFlipCommand(PPUContext& ctx, u32 context, u32 id)
{
    if (id > 7) {
        PS_LOG_ERROR(kChan, "cellGcmSetFlipCommand: buffer id {} > 7", id);
        return;
    }
    prepare_flip(thread_of(ctx).kernel.base(), context, id);
}

void _cellGcmSetFlipCommandWithWaitLabel(PPUContext& ctx, u32 context, u32 id, u32 label_index, u32 label_value)
{
    u8* base = thread_of(ctx).kernel.base();
    // NV406E_SEMAPHORE_OFFSET then NV406E_SEMAPHORE_ACQUIRE: the RSX waits until the label holds label_value.
    const u32 current = ps_read32(base, context + ctx_current);
    ps_write32(base, current, rsx::cmd::method_header(rsx::m::NV406E_SEMAPHORE_OFFSET, 2));
    ps_write32(base, current + 4, label_index * 0x10);
    ps_write32(base, current + 8, label_value);
    ps_write32(base, context + ctx_current, current + 12);
    if (id > 7) {
        PS_LOG_ERROR(kChan, "cellGcmSetFlipCommandWithWaitLabel: buffer id {} > 7", id);
        return;
    }
    prepare_flip(base, context, id);
}

// RPCS3: the handler takes effect once the RSX is initialized.
void cellGcmSetVBlankHandler(PPUContext&, u32 handler)
{
    GcmState& g = gcm();
    if (!g.initialized) {
        PS_LOG_WARN(kChan, "cellGcmSetVBlankHandler({:#x}) before cellGcmInit: ignored (as RPCS3)", handler);
        return;
    }
    g.vblank_handler.store(handler, std::memory_order_release);
    PS_LOG_INFO(kChan, "cellGcmSetVBlankHandler({:#x})", handler);
}

// ------------------------------------------------------------------------------------------------- tiles and Z-cull

// RPCS3 GcmTileInfo::pack (Emu/RSX/GCM.h): {tile, limit, pitch, format}.
std::array<u32, 4> pack_tile(const GcmState::Tile& t)
{
    return {(t.location + 1) | (t.bank << 4) | ((t.offset / 0x10000) << 16) | (t.location << 31),
        ((t.offset + t.size - 1) / 0x10000) << 16 | (t.location << 31), (t.pitch / 0x100) << 8,
        t.base | ((t.base + ((t.size - 1) / 0x10000)) << 13) | (t.comp << 26) | (1u << 30)};
}

s32 cellGcmSetTileInfo(PPUContext& ctx, u8 index, u8 location, u32 offset, u32 size, u32 pitch, u8 comp, u16 base_tag,
    u8 bank)
{
    PS_LOG_INFO(kChan, "cellGcmSetTileInfo(index {}, location {}, offset {:#x}, size {:#x}, pitch {:#x}, comp {}, "
                       "base {}, bank {})",
        index, location, offset, size, pitch, comp, base_tag, bank);
    if (index >= kTilesCount || base_tag >= 2048 || bank >= 4) return error(CELL_GCM_ERROR_INVALID_VALUE);
    if ((offset & 0xffff) || (size & 0xffff) || (pitch & 0xff)) return error(CELL_GCM_ERROR_INVALID_ALIGNMENT);
    if (location >= 2 || (comp != 0 && (comp < 7 || comp > 12))) return error(CELL_GCM_ERROR_INVALID_ENUM);
    if (comp) PS_LOG_ERROR(kChan, "cellGcmSetTileInfo: compression mode {} (as RPCS3: not emulated)", comp);
    GcmState& g = gcm();
    GcmState::Tile& t = g.tile[index];
    t = {location, offset, size, pitch, comp, base_tag, bank};
    const auto packed = pack_tile(t);
    u8* base = thread_of(ctx).kernel.base();
    for (u32 i = 0; i < 4; i++) ps_write32(base, g.tiles + 16 * index + 4 * i, packed[i]);
    return lv2::CELL_OK;
}

// Binding goes to lv2 with the packed words (RPCS3's HLE only sets the renderer's flag; lv2 decodes the same words).
s32 set_tile_binding(PPUContext& ctx, u8 index, bool bound)
{
    if (index >= kTilesCount) return error(CELL_GCM_ERROR_INVALID_VALUE);
    const auto p = pack_tile(gcm().tile[index]);
    return lv2::sys_rsx_context_attribute(thread_of(ctx), kContextId, 0x300, index,
        bound ? (u64{p[0]} << 32) | p[1] : 0, bound ? (u64{p[2]} << 32) | p[3] : 0, 0);
}

s32 cellGcmBindTile(PPUContext& ctx, u8 index) { return set_tile_binding(ctx, index, true); }

s32 cellGcmUnbindTile(PPUContext& ctx, u8 index) { return set_tile_binding(ctx, index, false); }

// cellGcmSetZcull: twelve arguments (the last four on the stack). RPCS3 GcmZcullInfo::pack, then lv2 0x301.
void cellGcmSetZcull(PPUContext& ctx, u8 index, u32 offset, u32 width, u32 height, u32 cull_start, u32 z_format,
    u32 aa_format, u32 zcull_dir, u32 zcull_format, u32 s_func, u32 s_ref, u32 s_mask)
{
    PS_LOG_INFO(kChan, "cellGcmSetZcull(index {}, offset {:#x}, {}x{}, cullStart {:#x}, zFormat {}, aaFormat {})",
        index, offset, width, height, cull_start, z_format, aa_format);
    if (index >= kZcullsCount) {
        PS_LOG_ERROR(kChan, "cellGcmSetZcull: index {} out of range", index);
        return;
    }
    const u32 region = 1 | (z_format << 4) | (aa_format << 8);
    const u32 size = ((width >> 6) << 22) | ((height >> 6) << 6);
    const u32 start = cull_start & ~0xFFFu;
    const u32 status0 =
        (zcull_dir << 1) | (zcull_format << 2) | ((s_func & 0xF) << 12) | (s_ref << 16) | (s_mask << 24);
    const u32 status1 = 0x2000 | (0x20 << 16);
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    const s32 r = lv2::sys_rsx_context_attribute(thread_of(ctx), kContextId, 0x301, index, (u64{region} << 32) | size,
        (u64{start} << 32) | offset, (u64{status0} << 32) | status1);
    if (r != lv2::CELL_OK) return;
    u8* base = thread_of(ctx).kernel.base();
    const u32 words[6] = {region, size, start, offset, status0, status1};
    for (u32 i = 0; i < 6; i++) ps_write32(base, g.zculls + 24 * index + 4 * i, words[i]);
}

u32 cellGcmGetTiledPitchSize(PPUContext&, u32 size)
{
    for (usize i = 0; i + 1 < kTiledPitches.size(); i++) {
        if (kTiledPitches[i] < size && size <= kTiledPitches[i + 1]) return kTiledPitches[i + 1];
    }
    return 0;
}

} // namespace

void register_cellGcmSys(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0x15BAE46B, _cellGcmInitBody),
        PS_HLE_FUNC(0x3A33C1FD, _cellGcmFunc15),
        PS_HLE_FUNC(0xE315A0B2, cellGcmGetConfiguration),
        PS_HLE_FUNC(0xA547ADDE, cellGcmGetControlRegister),
        PS_HLE_FUNC(0xF80196C1, cellGcmGetLabelAddress),
        PS_HLE_FUNC(0x8572BCE2, cellGcmGetReportDataAddressLocation),
        PS_HLE_FUNC(0xA6B180AC, cellGcmGetReportDataLocation),
        PS_HLE_FUNC(0x2AD4951B, cellGcmGetTimeStampLocation),
        PS_HLE_FUNC(0x21AC3697, cellGcmAddressToOffset),
        PS_HLE_FUNC(0x2A6FBA9C, cellGcmIoOffsetToAddress),
        PS_HLE_FUNC(0x626E8518, cellGcmMapEaIoAddressWithFlags),
        PS_HLE_FUNC(0xA53D12AE, cellGcmSetDisplayBuffer),
        PS_HLE_FUNC(0x21397818, _cellGcmSetFlipCommand),
        PS_HLE_FUNC(0xD8F88E1A, _cellGcmSetFlipCommandWithWaitLabel),
        PS_HLE_FUNC(0xA91B0402, cellGcmSetVBlankHandler),
        PS_HLE_FUNC(0xBD100DBC, cellGcmSetTileInfo),
        PS_HLE_FUNC(0x4524CCCD, cellGcmBindTile),
        PS_HLE_FUNC(0xD9B7653E, cellGcmUnbindTile),
        PS_HLE_FUNC(0xD34A420D, cellGcmSetZcull),
        PS_HLE_FUNC(0x055BD74D, cellGcmGetTiledPitchSize),
        PS_HLE_INTERNAL(NID_cellGcmCallback, cellGcmCallback),
        PS_HLE_INTERNAL(NID_interrupt_thread, _pspocket_gcm_interrupt_thread),
    });
}

} // namespace pspocket::hle
