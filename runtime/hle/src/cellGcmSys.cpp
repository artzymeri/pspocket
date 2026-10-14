// cellGcmSys (libgcm_sys): RSX setup for the game's command buffer library. The game links libgcm's command-writing
// part statically and calls these to set up the RSX, map main memory for it and register display buffers; it then
// writes commands into its command buffer and advances the put register itself. Everything here sits on lv2's
// sys_rsx (runtime/lv2/src/rsx.cpp), as in RPCS3, whose RSX front end runs the commands. Every function RPCS3
// implements is here, each citing RPCS3's (D-170); those RPCS3 leaves as bare TODO or UNIMPLEMENTED stubs are not
// (the cursor, GPAD, the "2" variants, _cellGcmFunc1/2/3/4/13/38 and the like).
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellGcmSys.cpp, Emu/RSX/GCM.h (structure layouts and packing).
//
// Guest structures (big-endian):
//   CellGcmContextData {u32 begin, end, current, callback}: the game's command buffer pointers. It lives at the start
//     of the RSX device area, as RPCS3 places it; callback is cellGcmCallback's descriptor.
//   CellGcmControl {u32 put, get, ref} = DMA control + 0x40.
//   CellGcmConfig {localAddress, ioAddress, localSize, ioSize, memoryFrequency, coreFrequency}.
//   Offset table: u16 ioAddress[3072] (EA MB -> IO MB) then u16 eaAddress[512] (IO MB -> EA MB), 0xFFFF = unmapped;
//     CellGcmOffsetTable {u32 ioAddress, eaAddress} points at the two halves. Beside it, on the host, the size of each
//     mapping by its first EA MB (RPCS3 IoMapTable): what an unmap unmaps.
//
// Flips are kept where the firmware keeps them, in lv2's driver information, head 1 (RsxDriverInfo::Head, RPCS3
// sys_rsx.h; RPCS3's HLE keeps the same in its renderer): at each flip lv2 sets the flip-done bit of flipFlags, the
// time and the buffer; cellGcmResetFlipStatus and cellGcmSetFlipStatus change the bit through lv2's 0x10a.
//
// The interrupt thread (_gcm_intr_thread, RPCS3 creates it in _cellGcmInitBody) is an lv2 PPU thread whose entry is
// an internal HLE function: every 1/59.94 s it counts a vblank in the driver information (lv2 package 0xFED, as
// RPCS3's vblank thread does) and calls the game's vblank handler with head 1. RPCS3 runs its handlers on that thread
// too, and so does this one: between vblanks it waits on an lv2 event queue of its own, where the RSX thread's
// listeners put an event for the game's flip handler (argument 1, the head), user handler (the command's argument)
// and queue handler (the head). Tile, Z-cull and flip mode (vsync) settings reach lv2, but the RSX front end does not
// use them yet.

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#include "gcm_api.hpp"
#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/core/timebase.h"
#include "pspocket/hle/display.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"
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

constexpr u32 CELL_GCM_LOCATION_LOCAL = 0, CELL_GCM_LOCATION_MAIN = 1; // anything but MAIN is local memory
constexpr u32 CELL_GCM_IOMAP_FLAG_STRICT_ORDERING = 1 << 1;
constexpr u32 CELL_GCM_DISPLAY_FLIP_STATUS_DONE = 0, CELL_GCM_DISPLAY_FLIP_STATUS_WAITING = 1;
constexpr s32 CELL_GCM_DEBUG_LEVEL0 = 1, CELL_GCM_DEBUG_LEVEL2 = 3;
constexpr u32 CELL_GCM_DISPLAY_FREQUENCY_59_94HZ = 1, CELL_GCM_DISPLAY_FREQUENCY_SCANOUT = 2;
constexpr u32 kContextId = 0x55555555;
constexpr u32 kLocalSize = 0x0F900000; // 249 MB: RPCS3's fixed value (its TODO: by SDK version, gcmGetLocalMemorySize)
constexpr u32 kTilesCount = 15, kZcullsCount = 8;
constexpr u32 kFragment = 32 * 1024; // the default command buffer's 32 KB segments
constexpr u32 kIoAddressEntries = 3072; // the offset table's ioAddress half: main memory below RSX local memory, by MB
constexpr u32 kEaAddressEntries = 512;  // its eaAddress half: the 512 MB IO address space, by MB
constexpr u32 NID_cellGcmCallback = 0x8DD39C9C;
constexpr u32 NID_interrupt_thread = 0x24A45AFC; // nid_of("_pspocket_gcm_interrupt_thread"): pspocket's own
constexpr auto kVblankPeriod = std::chrono::microseconds(16683); // 59.94 Hz
constexpr u64 CELL_GCM_SYSTEM_MODE_IOMAP_512MB = 1;
constexpr u64 kIntrFlip = 1, kIntrUser = 2, kIntrQueue = 3; // the interrupt thread's event sources

// CellGcmContextData
constexpr u32 ctx_begin = 0x0, ctx_end = 0x4, ctx_current = 0x8, ctx_callback = 0xC;
// CellGcmReportData {u64 timer; u32 value; u32 pad}
constexpr u32 kReportsReportOffset = 0x1400;
// RsxDriverInfo (RPCS3 sys_rsx.h): head 1, the head the HLE GCM drives, and the fields lv2 writes at each flip.
constexpr u32 kDriverHead1 = 0x10B8 + 1 * 0x40;
constexpr u32 head_lastFlipTime = 0x0, head_flipFlags = 0x8, head_flipBufferId = 0x10;
constexpr u32 kFlipDone = 0x80000000; // flipFlags: a flip ran (lv2 sets it at each flip; 0x10a clears it)

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
    u32 ctxt_addr = 0;    // the game's CellGcmContextData* variable (_cellGcmInitBody's argument)
    u32 control_addr = 0; // CellGcmControl
    u32 label_addr = 0;   // RSX reports area: labels, notifies, reports
    u32 driver_info = 0;  // lv2's RsxDriverInfo (the flips, head 1)
    u32 offset_table = 0; // u16 ioAddress[3072], then u16 eaAddress[512]
    std::array<u16, kIoAddressEntries> io_map_size{}; // RPCS3 IoMapTable: the MBs of the mapping at each EA MB
    u32 display_info = 0; // CellGcmDisplayInfo[8]
    u32 zculls = 0;       // CellGcmZcullInfo[8]
    u32 tiles = 0;        // CellGcmTileInfo[15]
    u32 cmd_begin = 0;    // the default command buffer (RPCS3 g_defaultCommandBufferBegin)
    u32 cmd_fragments = 0;
    // RPCS3 gcmInfo::command_size and segment_size (words), which only their getters read.
    std::atomic<u32> command_size{0x400}, segment_size{0x100};
    std::atomic<u32> main_mem_size{0}; // the IO address space: 0 until cellGcmInit (RPCS3's renderer), then 256/512 MB
    std::atomic<u32> reserved_size{0}; // cellGcmReserveIoMapSize
    u64 system_mode = 0; // cellGcmInitSystemMode
    std::atomic<s32> debug_level{CELL_GCM_DEBUG_LEVEL0}; // cellGcmSetDebugOutputLevel (nothing reads it, as in RPCS3)
    u32 intr_queue = 0;  // the interrupt thread's event queue (flip, user and queue handler events)
    std::atomic<u32> vblank_handler{0};
    std::atomic<u32> flip_handler{0};
    std::atomic<u32> user_handler{0};
    std::atomic<u32> queue_handler{0};
    std::atomic<u64> vblanks{0};
    struct Tile {
        u32 location = 0, offset = 0, size = 0, pitch = 0, comp = 0, base = 0, bank = 0;
    };
    std::array<Tile, kTilesCount> tile{};
    std::array<std::array<u32, 6>, kZcullsCount> zcull{}; // each region's packed CellGcmZcullInfo, as lv2 last took it
    // libgcm_sys's own state the D-519 functions keep (Sony's names in its comments): the tile words
    // cellGcmSetInvalidateTile sends (cellGcmSetTile's tile word without its location bits), the ring of buffers
    // cellGcmSetPrepareFlip queued (read and write indices, 8 entries), head 0's flip and queue handlers and the
    // second vblank handlers, and the hardware cursor.
    std::array<u32, kTilesCount> invalidate_tile{};
    std::mutex ring_mutex; // the ring is changed on PPU threads and, by flips, on the RSX thread; never held across lv2
    s32 ring_read = 0, ring_write = 0;
    std::array<u8, 8> ring{};
    std::atomic<u32> flip_handler_head0{0}, second_vhandler[2] = {0, 0};
    std::atomic<bool> second_v_enabled{false};
    struct Cursor {
        bool initialized = false, enabled = false, enable_dirty = false, image_dirty = false, mode_dirty = false;
        u32 offset = 0, x = 0, y = 1, mode = 1;
    } cursor; // under mutex
};

GcmState& gcm() { return HleProcess::current()->state<GcmState>(); }

// The events lv2 sends libgcm_sys (RsxDriverInfo::handlers, SYS_RSX_EVENT_*): a handler setter sets or clears its bit
// as libgcm_sys does (vblank 1/2, flip 8/0x10, queue 0x20/0x40, user 0x80, second vblank 0x400/0x800 for heads 0/1).
void set_event_mask(PPUContext& ctx, u32 bit, bool on)
{
    const u32 di = gcm().driver_info;
    if (!di) return; // libgcm_sys writes through a null driver information pointer here
    u8* base = guest_base(ctx);
    const u32 mask = ps_read32(base, di + 0x12C0); // RsxDriverInfo::handlers
    ps_write32(base, di + 0x12C0, on ? mask | bit : mask & ~bit);
}

// libgcm_sys's conversion of timebase ticks to microseconds, in two parts past 2^64 / 10^6 ticks.
u64 ticks_to_us(u64 ticks)
{
    constexpr u64 kSplit = 0x10C6F7A0B5EDull; // 2^64 / 10^6, rounded down
    const u64 freq = PS_TIMEBASE_HZ;
    u64 whole = 0;
    if (ticks > kSplit) {
        const u64 q = ticks / kSplit;
        whole = q * ((kSplit * 1'000'000ull) / freq);
        ticks -= q * kSplit;
    }
    return whole + ticks * 1'000'000ull / freq;
}


// The IO page of a main-memory MB (0xFFFF: not mapped). The table covers EAs below RSX local memory (3072 MB).
u32 io_address_entry(u8* base, const GcmState& g, u32 ea_mb)
{
    return ea_mb < kIoAddressEntries ? ps_read16(base, g.offset_table + 2 * ea_mb) : 0xFFFF;
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

// RPCS3 gcmMapEaIoAddress (the caller holds the GCM lock): lv2 maps it, then both halves of the offset table and the
// mapping's size by its first EA MB (RPCS3 IoMapTable, what cellGcmUnmapIoAddress unmaps). The table covers main
// memory below 3 GB: a range past it is CELL_GCM_ERROR_FAILURE before lv2 sees it (RPCS3 writes past its table).
// Before cellGcmInit there is no table: CELL_EINVAL, what lv2 answers then (RPCS3's lv2 too).
s32 map_ea_io(PPUContext& ctx, GcmState& g, u32 ea, u32 io, u32 size, bool strict)
{
    if (!size || (ea & 0xFFFFF) || (io & 0xFFFFF) || (size & 0xFFFFF)) return error(CELL_GCM_ERROR_FAILURE);
    if (((u64{ea} + size) >> 20) > kIoAddressEntries) {
        PS_LOG_ERROR(kChan, "mapping {:#x} bytes at {:#010x} for the RSX: past the main memory the offset table covers "
                            "-> CELL_GCM_ERROR_FAILURE",
            size, ea);
        return error(CELL_GCM_ERROR_FAILURE);
    }
    if (!g.offset_table) return lv2::err(lv2::CELL_EINVAL);
    lv2::PpuThread& t = thread_of(ctx);
    if (const s32 r = static_cast<s32>(
            lv2_syscall(ctx, 672, {kContextId, io, ea, size, 0xe000000000000800ull | (u64{strict} << 60)}));
        r != lv2::CELL_OK) {
        return r;
    }
    u8* base = t.kernel.base();
    const u32 ea_mb = ea >> 20, io_mb = io >> 20, count = size >> 20;
    for (u32 i = 0; i < count; i++) {
        ps_write16(base, g.offset_table + 2 * (ea_mb + i), static_cast<u16>(io_mb + i));
        ps_write16(base, g.offset_table + 2 * (kIoAddressEntries + io_mb + i), static_cast<u16>(ea_mb + i));
    }
    g.io_map_size[ea_mb] = static_cast<u16>(count);
    return lv2::CELL_OK;
}

// RPCS3 GcmUnmapIoAddress (the caller holds the GCM lock): the mapping that starts at IO offset `io`, its size from
// IoMapTable, is unmapped in lv2, then in both halves of the offset table. An offset that is not where a mapping
// starts is CELL_GCM_ERROR_FAILURE; so are an offset past the 512 MB IO space and an unmapped one, where RPCS3 reads
// past its tables.
s32 unmap_io(PPUContext& ctx, GcmState& g, u32 io)
{
    const u32 io_mb = io >> 20;
    if (!g.offset_table || io_mb >= kEaAddressEntries) return error(CELL_GCM_ERROR_FAILURE);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 ea_mb = ps_read16(base, g.offset_table + 2 * (kIoAddressEntries + io_mb));
    if (ea_mb >= kIoAddressEntries) return error(CELL_GCM_ERROR_FAILURE); // 0xFFFF: not mapped
    const u32 count = g.io_map_size[ea_mb];
    if (!count || io_mb + count > kEaAddressEntries) return error(CELL_GCM_ERROR_FAILURE);
    if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 673, {kContextId, u64{io_mb} << 20, u64{count} << 20}));
        r != lv2::CELL_OK) {
        return r;
    }
    for (u32 i = 0; i < count; i++) {
        ps_write16(base, g.offset_table + 2 * (ea_mb + i), 0xFFFF);
        ps_write16(base, g.offset_table + 2 * (kIoAddressEntries + io_mb + i), 0xFFFF);
    }
    g.io_map_size[ea_mb] = 0;
    return lv2::CELL_OK;
}

// A guest pointer argument a function writes through, checked first (D-042; RPCS3 writes unchecked).
bool out_pointer_ok(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (guest_readable(t, addr, size)) return true;
    log_call_site(ctx, t.kernel.base(), std::format("{}: {:#x} is not guest memory", what, addr));
    return false;
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

constexpr std::string_view handler_name(u64 source)
{
    return source == kIntrFlip ? "flip" : source == kIntrUser ? "user" : "queue";
}

// The body of _gcm_intr_thread (see the top of the file). Never returns: the thread unwinds from its sleep when the
// process ends.
void _pspocket_gcm_interrupt_thread(PPUContext& ctx, u64)
{
    GcmState& g = gcm();
    lv2::PpuThread& t = thread_of(ctx);
    auto next = std::chrono::steady_clock::now();
    bool logged = false;
    std::array<bool, 4> handler_logged{}; // by event source
    for (;;) {
        // On the screen's refresh when the platform reports it (D-971), else every 1/59.94 s.
        next = next_vblank(next, next + kVblankPeriod, std::chrono::steady_clock::now());
        // Until the vblank: the flip, user and queue handlers the RSX thread asks for, on this thread (as RPCS3's).
        for (auto now = std::chrono::steady_clock::now(); next > now; now = std::chrono::steady_clock::now()) {
            const u64 us = static_cast<u64>(
                std::max<s64>(1, std::chrono::duration_cast<std::chrono::microseconds>(next - now).count()));
            if (!g.intr_queue) {
                lv2::sys_timer_usleep(t, us);
                break;
            }
            if (lv2::sys_event_queue_receive(t, g.intr_queue, 0, us) != lv2::CELL_OK) break; // the vblank is due
            const u64 source = ctx.gpr[4];
            const u32 arg = static_cast<u32>(ctx.gpr[5]);
            const std::atomic<u32>* slot = source == kIntrFlip ? &g.flip_handler
                                         : source == kIntrUser ? &g.user_handler
                                         : source == kIntrQueue ? &g.queue_handler
                                                                : nullptr;
            const u32 handler = slot ? slot->load(std::memory_order_acquire) : 0;
            if (!handler) continue;
            if (!handler_logged[source]) {
                PS_LOG_INFO(kChan, "first call of the {} handler {:#x} ({:#x})", handler_name(source), handler, arg);
                handler_logged[source] = true;
            }
            call_guest(ctx, handler, {arg});
        }
        if (const auto now = std::chrono::steady_clock::now(); now - next > std::chrono::milliseconds(100)) {
            next = now; // fell far behind (a debugger, a stalled host): no burst of vblanks
        }
        lv2::sys_rsx_context_attribute(t, kContextId, 0xFED, 1, ps_timebase_now(), 0, 0);
        g.vblanks.fetch_add(1, std::memory_order_relaxed);
        if (const u32 handler = g.vblank_handler.load(std::memory_order_acquire)) {
            if (!logged) {
                PS_LOG_INFO(kChan, "first call of the vblank handler {:#x}", handler);
                logged = true;
            }
            call_guest(ctx, handler, {1});
        }
        if (g.second_v_enabled.load(std::memory_order_acquire)) {
            if (const u32 handler = g.second_vhandler[1].load(std::memory_order_acquire)) call_guest(ctx, handler, {1});
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
    // sys_rsx_context_allocate). Local memory cellGcmMapLocalMemory allocated is kept (RPCS3 allocates it again and
    // ignores the failure).
    StackVar out(ctx, base, 0x40);
    const u32 v = out.addr();
    if (!g.local_size) {
        if (const s32 r = lv2::sys_rsx_memory_allocate(t, v, v + 8, kLocalSize, 0, 0, 0, 0); r != lv2::CELL_OK) {
            PS_LOG_ERROR(kChan, "_cellGcmInitBody: RSX local memory: {}", lv2::error_name(r));
            return error(CELL_GCM_ERROR_FAILURE);
        }
    }
    if (const s32 r = lv2::sys_rsx_device_map(t, v + 0x10, 0, 8); r != lv2::CELL_OK) return r;
    const u32 device = static_cast<u32>(ps_read64(base, v + 0x10));
    if (const s32 r = lv2::sys_rsx_context_allocate(t, v + 0x18, v + 0x20, v + 0x28, v + 0x30, 0, g.system_mode);
        r != lv2::CELL_OK) {
        return r;
    }
    g.main_mem_size = g.system_mode == CELL_GCM_SYSTEM_MODE_IOMAP_512MB ? 0x20000000 : 0x10000000;
    const u32 dma_control = static_cast<u32>(ps_read64(base, v + 0x20));
    g.driver_info = static_cast<u32>(ps_read64(base, v + 0x28));
    const u32 reports = static_cast<u32>(ps_read64(base, v + 0x30));

    // InitOffsetTable, and the arrays cellGcmSys keeps for the game (RPCS3 vm::alloc in vm::main).
    MainMemory& mm = p.main_memory();
    g.offset_table = mm.alloc((kIoAddressEntries + kEaAddressEntries) * 2);
    g.display_info = mm.alloc(16 * 8);
    g.zculls = mm.alloc(24 * kZcullsCount);
    g.tiles = mm.alloc(16 * kTilesCount);
    if (!g.offset_table || !g.display_info || !g.zculls || !g.tiles) return error(CELL_GCM_ERROR_FAILURE);
    std::memset(base + g.offset_table, 0xFF, (kIoAddressEntries + kEaAddressEntries) * 2);

    g.local_addr = PS_MEM_RSX_LOCAL_BASE;
    g.local_size = kLocalSize;
    if (const s32 r = map_ea_io(ctx, g, io_address, 0, io_size, false); r != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "_cellGcmInitBody: mapping {:#x} bytes at {:#010x} for the RSX failed: {}", io_size,
            io_address, lv2::error_name(r));
        return error(CELL_GCM_ERROR_FAILURE);
    }
    // As libgcm_sys (D-519): the FIFO at the command buffer's start (IO offset 0x1000), both heads' flip status DONE
    // (0x10a), and the head-1 vblank event on.
    lv2_syscall(ctx, 674, {kContextId, 0x001, 0x1000, 0x1000, 0, 0});
    for (const u64 head : {0, 1}) lv2_syscall(ctx, 674, {kContextId, 0x10a, head, 0, kFlipDone, 0});
    ps_write32(base, g.driver_info + 0x12C0, ps_read32(base, g.driver_info + 0x12C0) | 4);
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
    g.ctxt_addr = context;
    g.initialized = true;

    PS_LOG_INFO(kChan, "GCM ready: local memory {:#010x} ({} MB), command buffer {:#010x} ({} segments), IO {} MB, "
                       "context {:#010x}, control {:#010x}, labels {:#010x}{}",
        g.local_addr, g.local_size >> 20, io_address, g.cmd_fragments, io_size >> 20, device, g.control_addr, reports,
        g.main_mem_size > 0x10000000 ? ", 512 MB IO address space" : "");

    // The interrupt thread's queue, and the RSX thread's listeners that feed it (flip, user and queue handlers).
    {
        const u32 q = v + 0x38, attr = v; // the context's outputs above are read: reuse the space
        ps_write32(base, attr, lv2::SYS_SYNC_FIFO);
        ps_write32(base, attr + 4, lv2::SYS_PPU_QUEUE);
        std::memcpy(base + attr + 8, "_gcmIntr", 8);
        if (const s32 r = lv2::sys_event_queue_create(t, q, attr, 0, 64); r != lv2::CELL_OK) {
            PS_LOG_ERROR(kChan, "_cellGcmInitBody: the interrupt queue: {} (no flip, user or queue handler will run)",
                lv2::error_name(r));
        } else {
            g.intr_queue = ps_read32(base, q);
        }
        lv2::Kernel& k = t.kernel;
        const u32 queue = g.intr_queue;
        const auto post = [&k, queue](u64 source, u32 arg) {
            if (!queue) return;
            if (const auto eq = k.event_queue(queue)) {
                if (eq->send(lv2::Event{source, arg, 0, 0}) != lv2::CELL_OK)
                    PS_LOG_WARN(kChan, "the interrupt queue is full: a {} handler call is dropped",
                        handler_name(source));
            }
        };
        k.set_rsx_flip_listener([&g, post](u32 buffer) {
            {
                // A flip of a queued buffer makes its slot the last flipped one (libgcm_sys's flip handling).
                std::lock_guard rl(g.ring_mutex);
                for (s32 at = g.ring_read; at != g.ring_write;) {
                    at = (at + 1) % 8;
                    if (g.ring[static_cast<u32>(at)] == buffer) {
                        g.ring_read = at;
                        break;
                    }
                }
            }
            if (g.flip_handler.load(std::memory_order_acquire)) post(kIntrFlip, 1);
        });
        k.set_rsx_user_command_listener([&g, post](u32 arg) {
            if (g.user_handler.load(std::memory_order_acquire)) post(kIntrUser, arg);
            else PS_LOG_WARN(kChan, "GCM_SET_USER_COMMAND {:#x} with no user handler set", arg);
        });
        k.set_rsx_queue_listener([&g, post](u32 head, u32) {
            if (g.queue_handler.load(std::memory_order_acquire)) post(kIntrQueue, head);
        });
    }
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

// RPCS3 cellGcmGetDefaultCommandWordSize and cellGcmGetDefaultSegmentWordSize: 0x400 and 0x100 words until
// cellGcmSetDefaultCommandBufferAndSegmentWordSize sets them.
u32 cellGcmGetDefaultCommandWordSize(PPUContext&) { return gcm().command_size.load(std::memory_order_relaxed); }

u32 cellGcmGetDefaultSegmentWordSize(PPUContext&) { return gcm().segment_size.load(std::memory_order_relaxed); }

// RPCS3 cellGcmInitDefaultFifoMode and cellGcmSetDefaultFifoSize: logged, CELL_OK (cellGcmInit alone sets up the
// default FIFO).
s32 cellGcmInitDefaultFifoMode(PPUContext&, s32 mode)
{
    PS_LOG_INFO(kChan, "cellGcmInitDefaultFifoMode({}): CELL_OK (as RPCS3)", mode);
    return lv2::CELL_OK;
}

s32 cellGcmSetDefaultFifoSize(PPUContext&, u32 buffer_size, u32 segment_size)
{
    PS_LOG_INFO(kChan, "cellGcmSetDefaultFifoSize(bufferSize {:#x}, segmentSize {:#x}): CELL_OK (as RPCS3)",
        buffer_size, segment_size);
    return lv2::CELL_OK;
}

// RPCS3 cellGcmSetDefaultCommandBuffer: the game's context variable (the one cellGcmInit filled) points at the default
// context again. Before cellGcmInit there is none: logged (RPCS3 writes to address 0).
void cellGcmSetDefaultCommandBuffer(PPUContext& ctx)
{
    const GcmState& g = gcm();
    if (!g.initialized) {
        PS_LOG_ERROR(kChan, "cellGcmSetDefaultCommandBuffer before cellGcmInit: there is no default context");
        return;
    }
    ps_write32(guest_base(ctx), g.ctxt_addr, g.context_addr);
}

// RPCS3 cellGcmSetDefaultCommandBufferAndSegmentWordSize(bufferSize, segmentSize): only while the default command
// buffer has run nothing (put and get both 0x1000, its first command), with room for two segments and a segment under
// 2^31 words, else CELL_GCM_ERROR_FAILURE; the sizes are kept for their getters (the default command buffer keeps its
// 32 KB segments, as in RPCS3). Before cellGcmInit: CELL_GCM_ERROR_FAILURE (RPCS3 reads a control register at 0).
s32 cellGcmSetDefaultCommandBufferAndSegmentWordSize(PPUContext& ctx, u32 buffer_size, u32 segment_size)
{
    PS_LOG_INFO(kChan, "cellGcmSetDefaultCommandBufferAndSegmentWordSize(bufferSize {:#x}, segmentSize {:#x})",
        buffer_size, segment_size);
    GcmState& g = gcm();
    if (!g.initialized) return error(CELL_GCM_ERROR_FAILURE);
    u8* base = guest_base(ctx);
    const u32 put = atomic_load_be32(base, g.control_addr), get = atomic_load_be32(base, g.control_addr + 4);
    if (put != 0x1000 || get != 0x1000 || buffer_size < segment_size * 2 || segment_size >= 0x80000000) {
        return error(CELL_GCM_ERROR_FAILURE);
    }
    g.command_size.store(buffer_size, std::memory_order_relaxed);
    g.segment_size.store(segment_size, std::memory_order_relaxed);
    return lv2::CELL_OK;
}

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

// RPCS3 cellGcmGetTimeStamp, cellGcmGetReport and cellGcmGetReportDataAddress: the same report entries in local memory
// only (an index past 2047 is logged and used anyway, as RPCS3).
u64 cellGcmGetTimeStamp(PPUContext& ctx, u32 index)
{
    return ps_read64(thread_of(ctx).kernel.base(), report_address(ctx, index, CELL_GCM_LOCATION_LOCAL));
}

// The report's value for a report type of 1..5 (CELL_GCM_ZPASS_PIXEL_CNT .. CELL_GCM_ZCULL_STATS3), else -1; the type
// selects nothing else (RPCS3).
u32 cellGcmGetReport(PPUContext& ctx, u32 type, u32 index)
{
    const u32 report = report_address(ctx, index, CELL_GCM_LOCATION_LOCAL);
    if (type < 1 || type > 5) return 0xFFFFFFFF;
    return ps_read32(thread_of(ctx).kernel.base(), report + 8);
}

u32 cellGcmGetReportDataAddress(PPUContext& ctx, u32 index)
{
    return report_address(ctx, index, CELL_GCM_LOCATION_LOCAL);
}

// RPCS3 _cellGcmFunc12: the local report area (CellGcmReportData[2048] at the reports area's 0x1400).
u32 _cellGcmFunc12(PPUContext&) { return gcm().label_addr + kReportsReportOffset; }

// RPCS3 cellGcmGetNotifyDataAddress(index): notify entries of 0x40 bytes in the main memory IO offset 241 MB maps, 0
// when nothing is mapped there (or before cellGcmInit, where RPCS3 reads a null table).
u32 cellGcmGetNotifyDataAddress(PPUContext& ctx, u32 index)
{
    const GcmState& g = gcm();
    if (!g.offset_table) return 0;
    const u32 entry = ps_read16(guest_base(ctx), g.offset_table + 2 * (kIoAddressEntries + 241));
    if (entry == 0xFFFF) return 0;
    return (entry << 20) + index * 0x40;
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

// RPCS3 cellGcmMapEaIoAddress: gcmMapEaIoAddress without strict ordering, under the GCM lock.
s32 cellGcmMapEaIoAddress(PPUContext& ctx, u32 ea, u32 io, u32 size)
{
    PS_LOG_INFO(kChan, "cellGcmMapEaIoAddress(ea {:#x}, io {:#x}, size {:#x})", ea, io, size);
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    return map_ea_io(ctx, g, ea, io, size, false);
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

// RPCS3 cellGcmMapMainMemory(ea, size, offset): `size` bytes at `ea` go to the first IO pages free for them below the
// reserved ones (an eaAddress entry past 0xBFF, i.e. 0xFFFF, is a free page), and *offset gets the IO offset. No room:
// CELL_GCM_ERROR_NO_IO_PAGE_TABLE. An unaligned or empty range: CELL_GCM_ERROR_FAILURE.
s32 cellGcmMapMainMemory(PPUContext& ctx, u32 ea, u32 size, u32 offset)
{
    PS_LOG_INFO(kChan, "cellGcmMapMainMemory(ea {:#x}, size {:#x}, offset {:#x})", ea, size, offset);
    if (!size || (ea & 0xFFFFF) || (size & 0xFFFFF)) return error(CELL_GCM_ERROR_FAILURE);
    if (!out_pointer_ok(ctx, offset, 4, "cellGcmMapMainMemory")) return error(CELL_GCM_ERROR_FAILURE);
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    u8* base = guest_base(ctx);
    const u32 end = (g.main_mem_size - g.reserved_size) >> 20; // 0 before cellGcmInit: no room
    for (u32 io = 0, count = 1; count + io <= end; count++) {
        if (ps_read16(base, g.offset_table + 2 * (kIoAddressEntries + io + count - 1)) > 0xBFF) {
            if (count >= (size >> 20)) {
                if (const s32 r = map_ea_io(ctx, g, ea, io << 20, size, false); r != lv2::CELL_OK) return r;
                ps_write32(base, offset, io << 20);
                return lv2::CELL_OK;
            }
        } else {
            io += count;
            count = 0;
        }
    }
    return error(CELL_GCM_ERROR_NO_IO_PAGE_TABLE);
}

// RPCS3 cellGcmMapLocalMemory(address, size): local memory is mapped once, so only before cellGcmInit (which maps it
// too) and only the first time; *address and *size get it. Else CELL_GCM_ERROR_FAILURE.
s32 cellGcmMapLocalMemory(PPUContext& ctx, u32 address, u32 size)
{
    PS_LOG_INFO(kChan, "cellGcmMapLocalMemory(address {:#x}, size {:#x})", address, size);
    if (!out_pointer_ok(ctx, address, 4, "cellGcmMapLocalMemory") ||
        !out_pointer_ok(ctx, size, 4, "cellGcmMapLocalMemory")) {
        return error(CELL_GCM_ERROR_FAILURE);
    }
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    if (g.local_addr || g.local_size) return error(CELL_GCM_ERROR_FAILURE);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    {
        StackVar out(ctx, base, 0x10);
        if (const s32 r = lv2::sys_rsx_memory_allocate(t, out.addr(), out.addr() + 8, kLocalSize, 0, 0, 0, 0);
            r != lv2::CELL_OK) {
            PS_LOG_ERROR(kChan, "cellGcmMapLocalMemory: RSX local memory: {}", lv2::error_name(r));
            return error(CELL_GCM_ERROR_FAILURE);
        }
    }
    g.local_addr = PS_MEM_RSX_LOCAL_BASE;
    g.local_size = kLocalSize;
    ps_write32(base, address, g.local_addr);
    ps_write32(base, size, g.local_size);
    return lv2::CELL_OK;
}

// RPCS3 cellGcmGetMaxIoMapSize: the IO address space (256 MB, 512 in CELL_GCM_SYSTEM_MODE_IOMAP_512MB, 0 before
// cellGcmInit as RPCS3's renderer) less what cellGcmReserveIoMapSize reserved.
u32 max_io_map_size(const GcmState& g) { return g.main_mem_size - g.reserved_size; }

u32 cellGcmGetMaxIoMapSize(PPUContext&) { return max_io_map_size(gcm()); }

// RPCS3 cellGcmReserveIoMapSize(size): keeps whole MBs at the top of the IO space from cellGcmMapMainMemory; a size not
// in MBs is CELL_GCM_ERROR_INVALID_ALIGNMENT, more than cellGcmGetMaxIoMapSize CELL_GCM_ERROR_INVALID_VALUE.
s32 cellGcmReserveIoMapSize(PPUContext&, u32 size)
{
    PS_LOG_INFO(kChan, "cellGcmReserveIoMapSize({:#x})", size);
    if (size & 0xFFFFF) return error(CELL_GCM_ERROR_INVALID_ALIGNMENT);
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    if (size > max_io_map_size(g)) return error(CELL_GCM_ERROR_INVALID_VALUE);
    g.reserved_size += size;
    return lv2::CELL_OK;
}

// RPCS3 cellGcmUnreserveIoMapSize(size): the same checks against what is reserved.
s32 cellGcmUnreserveIoMapSize(PPUContext&, u32 size)
{
    PS_LOG_INFO(kChan, "cellGcmUnreserveIoMapSize({:#x})", size);
    if (size & 0xFFFFF) return error(CELL_GCM_ERROR_INVALID_ALIGNMENT);
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    if (size > g.reserved_size) return error(CELL_GCM_ERROR_INVALID_VALUE);
    g.reserved_size -= size;
    return lv2::CELL_OK;
}

// RPCS3 cellGcmUnmapEaIoAddress(ea): the mapping of the EA's MB (the low 20 bits are ignored), by its IO offset; an EA
// at or past 3 GB, or not mapped, is CELL_GCM_ERROR_FAILURE.
s32 cellGcmUnmapEaIoAddress(PPUContext& ctx, u32 ea)
{
    PS_LOG_INFO(kChan, "cellGcmUnmapEaIoAddress({:#x})", ea);
    const u32 ea_mb = ea >> 20;
    if (ea_mb >= kIoAddressEntries) return error(CELL_GCM_ERROR_FAILURE);
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    if (!g.offset_table) return error(CELL_GCM_ERROR_FAILURE); // before cellGcmInit (RPCS3 reads a null table)
    const u32 io = u32{ps_read16(guest_base(ctx), g.offset_table + 2 * ea_mb)} << 20;
    if (io >= g.main_mem_size) return error(CELL_GCM_ERROR_FAILURE);
    return unmap_io(ctx, g, io);
}

// RPCS3 cellGcmUnmapIoAddress(io): the mapping that starts at that IO offset.
s32 cellGcmUnmapIoAddress(PPUContext& ctx, u32 io)
{
    PS_LOG_INFO(kChan, "cellGcmUnmapIoAddress({:#x})", io);
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    return unmap_io(ctx, g, io);
}

// RPCS3 cellGcmGetOffsetTable(table): CellGcmOffsetTable {ioAddress, eaAddress}, both 0 before cellGcmInit.
void cellGcmGetOffsetTable(PPUContext& ctx, u32 table)
{
    if (!out_pointer_ok(ctx, table, 8, "cellGcmGetOffsetTable")) return;
    const GcmState& g = gcm();
    u8* base = guest_base(ctx);
    ps_write32(base, table, g.offset_table);
    ps_write32(base, table + 4, g.offset_table ? g.offset_table + 2 * kIoAddressEntries : 0);
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
    return static_cast<s32>(lv2_syscall(ctx, 674, {kContextId, 0x104, id, (u64{width} << 32) | height,
        (u64{pitch} << 32) | offset, 0}));
}

// RPCS3 cellGcmGetDisplayInfo, cellGcmGetTileInfo and cellGcmGetZcullInfo: the arrays cellGcmSys keeps for the game
// (CellGcmDisplayInfo[8], CellGcmTileInfo[15], CellGcmZcullInfo[8]); 0 before cellGcmInit.
u32 cellGcmGetDisplayInfo(PPUContext&) { return gcm().display_info; }

u32 cellGcmGetTileInfo(PPUContext&) { return gcm().tiles; }

u32 cellGcmGetZcullInfo(PPUContext&) { return gcm().zculls; }

// The driver information's head 1, where lv2 keeps the flips; 0 before cellGcmInit.
u32 head1(const GcmState& g) { return g.driver_info ? g.driver_info + kDriverHead1 : 0; }

// RPCS3 cellGcmGetFlipStatus: CELL_GCM_DISPLAY_FLIP_STATUS_DONE (0) after a flip, WAITING (1) after
// cellGcmResetFlipStatus until the next flip; DONE from cellGcmInit on (RPCS3 rsx::thread::init) and before it.
u32 cellGcmGetFlipStatus(PPUContext& ctx)
{
    const u32 head = head1(gcm());
    if (!head) return CELL_GCM_DISPLAY_FLIP_STATUS_DONE;
    return (atomic_load_be32(guest_base(ctx), head + head_flipFlags) & kFlipDone) ? CELL_GCM_DISPLAY_FLIP_STATUS_DONE
                                                                                 : CELL_GCM_DISPLAY_FLIP_STATUS_WAITING;
}

// cellGcmResetFlipStatus and cellGcmSetFlipStatus, as libgcm_sys: head 1's flip flags through lv2's 0x10a
// (flipFlags = (flipFlags & a4) | a5): reset keeps all but the flip-done bit; set leaves only that bit (D-519). Before
// cellGcmInit there is no driver information and nothing is sent.
void set_flip_status(PPUContext& ctx, u32 head, u32 keep, u32 set)
{
    if (!gcm().driver_info) return;
    lv2_syscall(ctx, 674, {kContextId, 0x10a, head, keep, set, 0});
}

void cellGcmResetFlipStatus(PPUContext& ctx) { set_flip_status(ctx, 1, ~kFlipDone, 0); }

void cellGcmSetFlipStatus(PPUContext& ctx) { set_flip_status(ctx, 1, 0, kFlipDone); }

// RPCS3 cellGcmGetCurrentDisplayBufferId(id): the buffer of the last flip (0 before one), written as a byte.
s32 cellGcmGetCurrentDisplayBufferId(PPUContext& ctx, u32 id)
{
    if (!out_pointer_ok(ctx, id, 1, "cellGcmGetCurrentDisplayBufferId")) return error(CELL_GCM_ERROR_FAILURE);
    u8* base = guest_base(ctx);
    const u32 head = head1(gcm());
    // lv2 writes the buffer at a flip, on the RSX thread (D-587).
    ps_write8(base, id, static_cast<u8>(head ? atomic_load_be32(base, head + head_flipBufferId) : 0));
    return lv2::CELL_OK;
}

// cellGcmGetLastFlipTime: the last flip's time in microseconds of the timebase (the clock of
// sys_time_get_system_time), from head 1's lastFlipTime, which lv2 writes in timebase ticks; converted as libgcm_sys
// converts it (D-519); 0 before the first flip. RPCS3 returns its host clock less one second (from the RSX's start
// until the first flip too), which matches no guest clock.
u64 cellGcmGetLastFlipTime(PPUContext& ctx)
{
    const u32 head = head1(gcm());
    return head ? ticks_to_us(atomic_load_be64(guest_base(ctx), head + head_lastFlipTime)) : 0; // written at a flip
}

// RPCS3 cellGcmGetVBlankCount: the vblanks since cellGcmInit, which the interrupt thread counts (RPCS3's vblank thread
// counts its renderer's).
u64 cellGcmGetVBlankCount(PPUContext&) { return gcm().vblanks.load(std::memory_order_relaxed); }

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

// The game's CellGcmContextData, checked before it is read (D-042; RPCS3 reads it unchecked).
bool context_readable(PPUContext& ctx, u32 context, std::string_view what)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (guest_readable(t, context, 16)) return true;
    log_call_site(ctx, t.kernel.base(),
        std::format("{}: the context {:#x} is not readable guest memory", what, context));
    return false;
}

// The space check of RPCS3's command functions (`ctxt->current + 2 >= ctxt->end`, in words): with fewer than three
// words left before end, the context's callback makes room, called through its descriptor with a count of 8 (RPCS3's
// "???"). For the default context that is cellGcmCallback (flush, then the next segment); a game's own context may
// have its own. Returns the callback's result: 0 when there was room or it made some. A descriptor that is not guest
// memory counts as a failed callback (CELL_GCM_ERROR_FAILURE) and is logged with the call site (D-042).
s32 make_room(PPUContext& ctx, u8* base, u32 context)
{
    if (ps_read32(base, context + ctx_current) + 8 < ps_read32(base, context + ctx_end)) return lv2::CELL_OK;
    const u32 callback = ps_read32(base, context + ctx_callback);
    if (!guest_readable(thread_of(ctx), callback, 8)) {
        log_call_site(ctx, base, std::format("the callback of GCM context {:#x} ({:#x}) is not guest memory", context,
            callback));
        return error(CELL_GCM_ERROR_FAILURE);
    }
    return static_cast<s32>(call_guest(ctx, callback, {context, 8}));
}

// cellGcmSetFlipMode(mode): head 1's display sync, as libgcm_sys sets it (set_flip_mode, D-519); lv2 takes vsync from
// it (CELL_GCM_DISPLAY_VSYNC, 2, reaches lv2 as 2 or 3).
void set_flip_mode(PPUContext& ctx, u32 mode, u32 head);
void cellGcmSetFlipMode(PPUContext& ctx, u32 mode)
{
    PS_LOG_INFO(kChan, "cellGcmSetFlipMode({})", mode);
    set_flip_mode(ctx, mode, 1);
}

// RPCS3 gcmSetPrepareFlip (the checked API, under cellGcmSetPrepareFlip and cellGcmSetFlip): a buffer id past 7 is
// CELL_GCM_ERROR_FAILURE; room for the command, where a callback that fails is logged, nothing is written and its
// result is returned as it is (RPCS3's not_an_error); then GCM_FLIP_COMMAND with the id. For the default context the
// put register then advances by the command's 8 bytes, so the RSX runs it: RPCS3 adds to put, it does not set put
// from current. Returns the id.
s32 set_prepare_flip(PPUContext& ctx, u32 context, u32 id, std::string_view what)
{
    if (id > 7) return error(CELL_GCM_ERROR_FAILURE);
    if (!context_readable(ctx, context, what)) return error(CELL_GCM_ERROR_FAILURE);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (const s32 res = make_room(ctx, base, context); res != 0) {
        PS_LOG_ERROR(kChan, "cellGcmSetPrepareFlip: callback failed ({:#010x})", static_cast<u32>(res));
        return res;
    }
    prepare_flip(base, context, id);
    GcmState& g = gcm();
    {
        // libgcm_sys's ring of queued buffers (D-519): the next slot takes the id. libgcm_sys refuses a full ring
        // (CELL_GCM_ERROR_FAILURE); here the flip command is written already, so the oldest slot is dropped instead.
        std::lock_guard rl(g.ring_mutex);
        const s32 next = (g.ring_write + 1) % 8;
        if (next == g.ring_read) {
            PS_LOG_WARN(kChan, "{}: eight flips queued; the oldest leaves libgcm_sys's flip ring", what);
            g.ring_read = (g.ring_read + 1) % 8;
        }
        g.ring_write = next;
        g.ring[static_cast<u32>(next)] = static_cast<u8>(id);
    }
    if (context == g.context_addr) {
        for (u32 put = atomic_load_be32(base, g.control_addr);;) {
            const u32 seen = atomic_cas_be32(base, g.control_addr, put, put + 8);
            if (seen == put) break;
            put = seen;
        }
        if (rsx::Rsx* rsx = t.kernel.rsx()) rsx->notify();
    }
    return static_cast<s32>(id);
}

// cellGcmSetPrepareFlip(context, id): RPCS3's returns gcmSetPrepareFlip's result: the id once the command is written.
s32 cellGcmSetPrepareFlip(PPUContext& ctx, u32 context, u32 id)
{
    return set_prepare_flip(ctx, context, id, "cellGcmSetPrepareFlip");
}

// cellGcmSetFlip(context, id): RPCS3: gcmSetPrepareFlip, whose negative result becomes CELL_GCM_ERROR_FAILURE and any
// other CELL_OK.
s32 cellGcmSetFlip(PPUContext& ctx, u32 context, u32 id)
{
    return set_prepare_flip(ctx, context, id, "cellGcmSetFlip") < 0 ? error(CELL_GCM_ERROR_FAILURE) : lv2::CELL_OK;
}

// NV406E_SEMAPHORE_OFFSET 0x10 and NV406E_SEMAPHORE_ACQUIRE 0 at current: the RSX waits until label 1, which a flip
// clears (rsx m_flip_command), is 0.
void write_wait_flip(u8* base, u32 context)
{
    const u32 current = ps_read32(base, context + ctx_current);
    ps_write32(base, current, rsx::cmd::method_header(rsx::m::NV406E_SEMAPHORE_OFFSET, 2));
    ps_write32(base, current + 4, 0x10);
    ps_write32(base, current + 8, 0);
    ps_write32(base, context + ctx_current, current + 12);
}

// cellGcmSetWaitFlip(context): RPCS3: room for the command (a callback that fails is logged and nothing is written),
// then the wait for label 1. The game flushes it (no put update).
void cellGcmSetWaitFlip(PPUContext& ctx, u32 context)
{
    if (!context_readable(ctx, context, "cellGcmSetWaitFlip")) return;
    u8* base = guest_base(ctx);
    if (const s32 res = make_room(ctx, base, context); res != 0) {
        PS_LOG_ERROR(kChan, "cellGcmSetWaitFlip(): callback failed ({:#010x})", static_cast<u32>(res));
        return;
    }
    write_wait_flip(base, context);
}

// cellGcmSetWaitFlipUnsafe(context): RPCS3: cellGcmSetWaitFlip's command without the room check.
void cellGcmSetWaitFlipUnsafe(PPUContext& ctx, u32 context)
{
    if (!context_readable(ctx, context, "cellGcmSetWaitFlipUnsafe")) return;
    write_wait_flip(guest_base(ctx), context);
}

// cellGcmSetUserCommand(context, cause): RPCS3: room for the command (a callback that fails is logged and nothing is
// written), then GCM_SET_USER_COMMAND with the cause. The game flushes it; when the RSX runs it, the interrupt thread
// calls the user handler with the cause.
void cellGcmSetUserCommand(PPUContext& ctx, u32 context, u32 cause)
{
    if (!context_readable(ctx, context, "cellGcmSetUserCommand")) return;
    u8* base = guest_base(ctx);
    if (const s32 res = make_room(ctx, base, context); res != 0) {
        PS_LOG_ERROR(kChan, "cellGcmSetUserCommand(): callback failed ({:#010x})", static_cast<u32>(res));
        return;
    }
    const u32 current = ps_read32(base, context + ctx_current);
    ps_write32(base, current, rsx::cmd::method_header(rsx::m::GCM_SET_USER_COMMAND, 1));
    ps_write32(base, current + 4, cause);
    ps_write32(base, context + ctx_current, current + 8);
}

s32 ring_lookup(GcmState& g, u32 index, bool advance);
s32 flip_immediate(PPUContext& ctx, u32 index, u32 head);

// cellGcmSetFlipImmediate(index), as libgcm_sys (D-519): the buffer at `index` of the flip ring, which becomes the last
// flipped slot, is flipped on head 1 at once (lv2's 0x102); CELL_GCM_ERROR_FAILURE when the ring does not hold it.
s32 cellGcmSetFlipImmediate(PPUContext& ctx, u8 index)
{
    s32 id = 0;
    {
        GcmState& g = gcm();
        std::lock_guard rl(g.ring_mutex);
        id = ring_lookup(g, index, true);
    }
    if (id < 0) return error(CELL_GCM_ERROR_FAILURE);
    return flip_immediate(ctx, static_cast<u32>(id) & 0xFF, 1);
}

// cellGcmSetSecondVFrequency(freq), as libgcm_sys: lv2's 0x108 for the second vblank (handler 2) of head 1, 59.94 Hz
// as 3, the scanout's as 2, anything else (CELL_GCM_DISPLAY_FREQUENCY_DISABLE) as 4, which turns it off. While it is
// on, the interrupt thread calls the second vblank handler with each vblank (every vblank here is 59.94 Hz).
void cellGcmSetSecondVFrequency(PPUContext& ctx, u32 freq)
{
    PS_LOG_INFO(kChan, "cellGcmSetSecondVFrequency({})", freq);
    const u32 sel = freq == CELL_GCM_DISPLAY_FREQUENCY_59_94HZ ? 3 : freq == CELL_GCM_DISPLAY_FREQUENCY_SCANOUT ? 2 : 4; // 4: DISABLE (3) or any other
    if (lv2_syscall(ctx, 674, {kContextId, 0x108, 1, sel, 2, 0}) == lv2::CELL_OK) {
        gcm().second_v_enabled.store(sel != 4, std::memory_order_release);
    }
}

// RPCS3 cellGcmSetDebugOutputLevel(level): CELL_GCM_DEBUG_LEVEL0..2 (1..3) are kept, anything else is ignored. Nothing
// reads it, as in RPCS3.
void cellGcmSetDebugOutputLevel(PPUContext&, s32 level)
{
    if (level < CELL_GCM_DEBUG_LEVEL0 || level > CELL_GCM_DEBUG_LEVEL2) {
        PS_LOG_WARN(kChan, "cellGcmSetDebugOutputLevel({}): not a debug level, ignored (as RPCS3)", level);
        return;
    }
    gcm().debug_level.store(level, std::memory_order_relaxed);
    PS_LOG_INFO(kChan, "cellGcmSetDebugOutputLevel({})", level);
}

// RPCS3 cellGcmTerminate: CELL_OK ("the firmware just returns CELL_OK as well").
s32 cellGcmTerminate(PPUContext&)
{
    PS_LOG_INFO(kChan, "cellGcmTerminate(): CELL_OK (as RPCS3 and the firmware)");
    return lv2::CELL_OK;
}

// RPCS3: the handler takes effect once the RSX is initialized.
// cellGcmSetGraphicsHandler(handler): RPCS3 notes it (cellGcmSys.todo) and keeps nothing: the RSX's graphics errors
// it would report never reach a game there or here. Sniper: Ghost Warrior sets one at boot (phone run of 2026-10-06).
void cellGcmSetGraphicsHandler(PPUContext&, u32 handler)
{
    PS_LOG_INFO(kChan, "cellGcmSetGraphicsHandler({:#x}): kept by nothing, as RPCS3", handler);
}

void cellGcmSetVBlankHandler(PPUContext& ctx, u32 handler)
{
    GcmState& g = gcm();
    if (!g.initialized) {
        PS_LOG_WARN(kChan, "cellGcmSetVBlankHandler({:#x}) before cellGcmInit: ignored (as RPCS3)", handler);
        return;
    }
    g.vblank_handler.store(handler, std::memory_order_release);
    set_event_mask(ctx, 2, handler != 0);
    PS_LOG_INFO(kChan, "cellGcmSetVBlankHandler({:#x})", handler);
}

// cellGcmSetFlipHandler(handler), as RPCS3: once the RSX is initialized, called on the interrupt thread with head 1
// after each flip.
void cellGcmSetFlipHandler(PPUContext& ctx, u32 handler)
{
    GcmState& g = gcm();
    if (!g.initialized) {
        PS_LOG_WARN(kChan, "cellGcmSetFlipHandler({:#x}) before cellGcmInit: ignored (as RPCS3)", handler);
        return;
    }
    g.flip_handler.store(handler, std::memory_order_release);
    set_event_mask(ctx, 0x10, handler != 0);
    PS_LOG_INFO(kChan, "cellGcmSetFlipHandler({:#x})", handler);
}

// cellGcmSetUserHandler(handler), as RPCS3: once the RSX is initialized, called on the interrupt thread with the
// argument of each GCM_SET_USER_COMMAND the RSX runs.
void cellGcmSetUserHandler(PPUContext& ctx, u32 handler)
{
    GcmState& g = gcm();
    if (!g.initialized) {
        PS_LOG_WARN(kChan, "cellGcmSetUserHandler({:#x}) before cellGcmInit: ignored (as RPCS3)", handler);
        return;
    }
    g.user_handler.store(handler, std::memory_order_release);
    set_event_mask(ctx, 0x80, handler != 0);
    PS_LOG_INFO(kChan, "cellGcmSetUserHandler({:#x})", handler);
}

// cellGcmSetQueueHandler(handler), as RPCS3: once the RSX is initialized, called on the interrupt thread with the head
// whenever the RSX queues a buffer: head 1 before the flip of each flip command (RPCS3 flip_command), and a
// GCM_DRIVER_QUEUE method's head (lv1's queue event, which the firmware's interrupt thread answers).
void cellGcmSetQueueHandler(PPUContext& ctx, u32 handler)
{
    GcmState& g = gcm();
    if (!g.initialized) {
        PS_LOG_WARN(kChan, "cellGcmSetQueueHandler({:#x}) before cellGcmInit: ignored (as RPCS3)", handler);
        return;
    }
    g.queue_handler.store(handler, std::memory_order_release);
    set_event_mask(ctx, 0x40, handler != 0);
    PS_LOG_INFO(kChan, "cellGcmSetQueueHandler({:#x})", handler);
}

// cellGcmInitSystemMode(mode), as RPCS3: kept for _cellGcmInitBody, where CELL_GCM_SYSTEM_MODE_IOMAP_512MB (1) makes
// the IO address space 512 MB instead of 256.
s32 cellGcmInitSystemMode(PPUContext&, u64 mode)
{
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    g.system_mode = mode;
    PS_LOG_INFO(kChan, "cellGcmInitSystemMode({:#x}){}", mode,
        mode == CELL_GCM_SYSTEM_MODE_IOMAP_512MB ? ": 512 MB IO address space" : "");
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- tiles and Z-cull

// RPCS3 GcmTileInfo::pack (Emu/RSX/GCM.h): {tile, limit, pitch, format}.
std::array<u32, 4> pack_tile(const GcmState::Tile& t)
{
    return {(t.location + 1) | (t.bank << 4) | ((t.offset / 0x10000) << 16) | (t.location << 31),
        ((t.offset + t.size - 1) / 0x10000) << 16 | (t.location << 31), (t.pitch / 0x100) << 8,
        t.base | ((t.base + ((t.size - 1) / 0x10000)) << 13) | (t.comp << 26) | (1u << 30)};
}

// RPCS3 cellGcmSetTileInfo, shared with cellGcmSetTile (whose checks RPCS3 copied from it): the checks, then the tile's
// packed CellGcmTileInfo in the array cellGcmSys keeps for the game.
s32 set_tile_info(PPUContext& ctx, std::string_view what, u8 index, u8 location, u32 offset, u32 size, u32 pitch,
    u8 comp, u16 base_tag, u8 bank)
{
    PS_LOG_INFO(kChan, "{}(index {}, location {}, offset {:#x}, size {:#x}, pitch {:#x}, comp {}, base {}, bank {})",
        what, index, location, offset, size, pitch, comp, base_tag, bank);
    if (index >= kTilesCount || base_tag >= 2048 || bank >= 4) return error(CELL_GCM_ERROR_INVALID_VALUE);
    if ((offset & 0xffff) || (size & 0xffff) || (pitch & 0xff)) return error(CELL_GCM_ERROR_INVALID_ALIGNMENT);
    if (location >= 2 || (comp != 0 && (comp < 7 || comp > 12))) return error(CELL_GCM_ERROR_INVALID_ENUM);
    if (comp) PS_LOG_ERROR(kChan, "{}: compression mode {} (as RPCS3: not emulated)", what, comp);
    GcmState& g = gcm();
    GcmState::Tile& t = g.tile[index];
    t = {location, offset, size, pitch, comp, base_tag, bank};
    const auto packed = pack_tile(t);
    u8* base = thread_of(ctx).kernel.base();
    for (u32 i = 0; i < 4; i++) ps_write32(base, g.tiles + 16 * index + 4 * i, packed[i]);
    return lv2::CELL_OK;
}

s32 cellGcmSetTileInfo(PPUContext& ctx, u8 index, u8 location, u32 offset, u32 size, u32 pitch, u8 comp, u16 base_tag,
    u8 bank)
{
    return set_tile_info(ctx, "cellGcmSetTileInfo", index, location, offset, size, pitch, comp, base_tag, bank);
}

// Binding goes to lv2 with the packed words (RPCS3's HLE only sets the renderer's flag; lv2 decodes the same words).
s32 set_tile_binding(PPUContext& ctx, u8 index, bool bound)
{
    if (index >= kTilesCount) return error(CELL_GCM_ERROR_INVALID_VALUE);
    const auto p = pack_tile(gcm().tile[index]);
    return static_cast<s32>(lv2_syscall(ctx, 674, {kContextId, 0x300, index,
        bound ? (u64{p[0]} << 32) | p[1] : 0, bound ? (u64{p[2]} << 32) | p[3] : 0, 0}));
}

s32 cellGcmBindTile(PPUContext& ctx, u8 index) { return set_tile_binding(ctx, index, true); }

s32 cellGcmUnbindTile(PPUContext& ctx, u8 index) { return set_tile_binding(ctx, index, false); }

// RPCS3 cellGcmSetTile: cellGcmSetTileInfo, then the tile is bound when its pitch is not 0 and unbound otherwise
// (through lv2, as cellGcmBindTile and cellGcmUnbindTile here).
s32 cellGcmSetTile(PPUContext& ctx, u8 index, u8 location, u32 offset, u32 size, u32 pitch, u8 comp, u16 base_tag,
    u8 bank)
{
    if (const s32 r = set_tile_info(ctx, "cellGcmSetTile", index, location, offset, size, pitch, comp, base_tag, bank);
        r != lv2::CELL_OK) {
        return r;
    }
    // What cellGcmSetInvalidateTile sends for it later: the tile word without its location bits (libgcm_sys, D-519).
    GcmState& g = gcm();
    g.invalidate_tile[index] = pack_tile(g.tile[index])[0] & ~3u;
    return set_tile_binding(ctx, index, pitch > 0);
}

// RPCS3 cellGcmSetZcull and cellGcmBindZcull: GcmZcullInfo::pack, then, under the GCM lock, lv2 0x301 (which refuses
// an index past 7, and, as RPCS3's lv2, a cull range outside the 3 MB of Z-cull RAM or an offset outside local
// memory); the packed CellGcmZcullInfo goes to the array cellGcmSys keeps for the game only when lv2 took it. Returns
// lv2's result. Twelve arguments, the last four on the stack.
s32 bind_zcull(PPUContext& ctx, std::string_view what, u8 index, u32 offset, u32 width, u32 height, u32 cull_start,
    u32 z_format, u32 aa_format, u32 zcull_dir, u32 zcull_format, u32 s_func, u32 s_ref, u32 s_mask)
{
    PS_LOG_INFO(kChan, "{}(index {}, offset {:#x}, {}x{}, cullStart {:#x}, zFormat {}, aaFormat {})", what, index,
        offset, width, height, cull_start, z_format, aa_format);
    const std::array<u32, 6> words = {
        1 | (z_format << 4) | (aa_format << 8),                                                             // region
        ((width >> 6) << 22) | ((height >> 6) << 6),                                                        // size
        cull_start & ~0xFFFu,                                                                               // start
        offset,                                                                                             // offset
        (zcull_dir << 1) | (zcull_format << 2) | ((s_func & 0xF) << 12) | (s_ref << 16) | (s_mask << 24), // status0
        0x2000 | (0x20 << 16),                                                                              // status1
    };
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 674, {kContextId, 0x301, index,
            (u64{words[0]} << 32) | words[1], (u64{words[2]} << 32) | words[3], (u64{words[4]} << 32) | words[5]}));
        r != lv2::CELL_OK) {
        return r;
    }
    if (index < kZcullsCount) { // lv2 took it, so it is
        g.zcull[index] = words;
        u8* base = guest_base(ctx);
        for (u32 i = 0; i < 6; i++) ps_write32(base, g.zculls + 24 * index + 4 * i, words[i]);
    }
    return lv2::CELL_OK;
}

void cellGcmSetZcull(PPUContext& ctx, u8 index, u32 offset, u32 width, u32 height, u32 cull_start, u32 z_format,
    u32 aa_format, u32 zcull_dir, u32 zcull_format, u32 s_func, u32 s_ref, u32 s_mask)
{
    if (const s32 r = bind_zcull(ctx, "cellGcmSetZcull", index, offset, width, height, cull_start, z_format, aa_format,
            zcull_dir, zcull_format, s_func, s_ref, s_mask);
        r != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "cellGcmSetZcull: lv2 refused Z-cull region {} ({})", index, lv2::error_name(r));
    }
}

s32 cellGcmBindZcull(PPUContext& ctx, u8 index, u32 offset, u32 width, u32 height, u32 cull_start, u32 z_format,
    u32 aa_format, u32 zcull_dir, u32 zcull_format, u32 s_func, u32 s_ref, u32 s_mask)
{
    return bind_zcull(ctx, "cellGcmBindZcull", index, offset, width, height, cull_start, z_format, aa_format,
        zcull_dir, zcull_format, s_func, s_ref, s_mask);
}

// RPCS3 cellGcmUnbindZcull(index): an index past 7 is CELL_GCM_ERROR_INVALID_VALUE; else the region is unbound and
// keeps its settings (RPCS3 clears its renderer's bound flag): lv2 0x301 gets the words it last took with status1 0,
// which it reads as unbound. Before cellGcmInit lv2 has no context: its CELL_EINVAL, as cellGcmUnbindTile.
s32 cellGcmUnbindZcull(PPUContext& ctx, u8 index)
{
    PS_LOG_INFO(kChan, "cellGcmUnbindZcull({})", index);
    if (index >= kZcullsCount) return error(CELL_GCM_ERROR_INVALID_VALUE);
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    const std::array<u32, 6>& w = g.zcull[index];
    return static_cast<s32>(lv2_syscall(ctx, 674, {kContextId, 0x301, index, (u64{w[0]} << 32) | w[1],
        (u64{w[2]} << 32) | w[3], u64{w[4]} << 32, 0}));
}

u32 cellGcmGetTiledPitchSize(PPUContext&, u32 size)
{
    for (usize i = 0; i + 1 < kTiledPitches.size(); i++) {
        if (kTiledPitches[i] < size && size <= kTiledPitches[i + 1]) return kTiledPitches[i + 1];
    }
    return 0;
}


// ------------------------------------------------------------------------------------------------- D-519
//
// The functions RPCS3 leaves as stubs, each as libgcm_sys.sprx (4.92) does it (tests/hle/test_gcm_compare.cpp runs
// Sony's module beside these on the same lv2). libgcm_sys reads lv2's driver information (RsxDriverInfo, the layout
// sys_rsx keeps here) and sends sys_rsx packages; these do the same through lv2's syscall table (lv2_syscall), so a
// test sees the same calls. "Head 1" is the head libgcm_sys drives by default (its setting is 1 unless changed, and
// nothing here changes it); the "2" variants are head 0's.

constexpr u32 kScRsxContextAttribute = 674, kScRsxAttribute = 677;
constexpr u32 kDriverHeads = 0x10B8;     // RsxDriverInfo::head[8], 0x40 each
constexpr u32 head_vblank_count = 0x30, head_last_vtime_low = 0x1C, head_last_vtime_high = 0x3C,
              head_last_second_vtime = 0x20, head_field = 0x38;

// libgcm_sys's context ID (0 before cellGcmInit) and lv2's driver information.
u32 context_id() { return gcm().driver_info ? kContextId : 0; }
u32 driver_info() { return gcm().driver_info; }

s32 rsx_attribute_call(PPUContext& ctx, std::initializer_list<u64> args)
{
    return static_cast<s32>(lv2_syscall(ctx, kScRsxContextAttribute, args));
}

// _cellGcmFunc1: the GPU version in the driver information. _cellGcmFunc2: libgcm_sys's first word, 0 once
// initialised. _cellGcmFunc13: the notify area (reports + reportsNotifyOffset). _cellGcmFunc38(u32* out): the driver
// information's last words (+0x12F0); CELL_GCM_ERROR_FAILURE before cellGcmInit. libgcm_sys reads address 4 before
// cellGcmInit (it faults): 0 here.
u32 _cellGcmFunc1(PPUContext& ctx)
{
    const u32 di = driver_info();
    return di ? ps_read32(guest_base(ctx), di + 4) : 0;
}
u32 _cellGcmFunc2(PPUContext&) { return 0; }
u32 _cellGcmFunc13(PPUContext& ctx)
{
    const u32 di = driver_info();
    return di ? gcm().label_addr + ps_read32(guest_base(ctx), di + 0x2C) : 0;
}
s32 _cellGcmFunc38(PPUContext& ctx, u32 out)
{
    const u32 di = driver_info();
    if (!di) return error(CELL_GCM_ERROR_FAILURE);
    if (!out_pointer_ok(ctx, out, 4, "_cellGcmFunc38")) return error(CELL_GCM_ERROR_FAILURE);
    ps_write32(guest_base(ctx), out, di + 0x12F0);
    return lv2::CELL_OK;
}

// _cellGcmFunc3(code): libgcm_sys's table of magic numbers (codes and the answers it gives), 0 for any other.
u32 _cellGcmFunc3(PPUContext&, u32 code)
{
    switch (code) {
    case 0xBBBB0000: return 0x31337303;
    case 0xBBBB0001: return 0x3137C0DE;
    case 0xBBBB1000: return 0x31337808;
    case 0xBBBB1001: return 0x3137AF00;
    case 0xBBBB2000: return 0x313371C3;
    case 0xBBBB2001: return 0x31337A73;
    case 0xBEEF2000: return 0x66604200;
    case 0xBEEF2002: return 0xFEED0000;
    case 0xBEEF2004: return 0x66626660;
    case 0xBEEF2005: return 0xFEED0001;
    case 0xBEEF2006: return 0x66606660;
    case 0xBEEF2007: return 0x66616661;
    case 0xBEEF2008: return 0x56616660;
    case 0xBEEF2009: return 0x56616661;
    case 0xBEEF4097: return 0x31337000;
    case 0xCCCC0001: return 0xCAFEBABE;
    default: return 0;
    }
}

// _cellGcmFunc4: package 0x106 (performance monitor) with 1. libgcm_sys sends it with whatever context it has.
s32 _cellGcmFunc4(PPUContext& ctx) { return rsx_attribute_call(ctx, {context_id(), 0x106, 1, 0, 0, 0}); }

// A head's fields of the driver information (0 before cellGcmInit), which lv2 writes at flips, queues and vblanks while
// the game runs: read whole, with acquire order (D-587).
u32 head_word(PPUContext& ctx, u32 head, u32 field)
{
    const u32 di = driver_info();
    return di ? atomic_load_be32(guest_base(ctx), di + kDriverHeads + 0x40 * head + field) : 0;
}
u64 head_dword(PPUContext& ctx, u32 head, u32 field)
{
    const u32 di = driver_info();
    return di ? atomic_load_be64(guest_base(ctx), di + kDriverHeads + 0x40 * head + field) : 0;
}

u32 cellGcmGetCurrentField(PPUContext& ctx) { return head_word(ctx, 1, head_field); }

// The flip status of head 0: 1 (waiting) until its flip-done bit is set.
u32 cellGcmGetFlipStatus2(PPUContext& ctx)
{
    const u32 di = driver_info();
    if (!di) return 0;
    return (atomic_load_be32(guest_base(ctx), di + kDriverHeads + head_flipFlags) >> 31) ^ 1;
}

u64 cellGcmGetVBlankCount2(PPUContext& ctx) { return head_dword(ctx, 0, head_vblank_count); }

// The second vblank's last time (head 1, timebase ticks) in microseconds.
u64 cellGcmGetLastSecondVTime(PPUContext& ctx)
{
    return driver_info() ? ticks_to_us(head_dword(ctx, 1, head_last_second_vtime)) : 0;
}

// The last vblank's time on head 1, as lv2 keeps it (two words, timebase ticks; not converted).
u64 cellGcmSysGetLastVBlankTime(PPUContext& ctx)
{
    return (u64{head_word(ctx, 1, head_last_vtime_high)} << 32) | head_word(ctx, 1, head_last_vtime_low);
}

// Head 0's last flip time in microseconds (lv2 keeps timebase ticks).
u64 cellGcmGetLastFlipTime2(PPUContext& ctx) { return driver_info() ? ticks_to_us(head_dword(ctx, 0, head_lastFlipTime)) : 0; }

// Flip status of head 0 through lv2's 0x10a: set (flipFlags | 0x80000000) or reset (flipFlags & 0x7FFFFFFF).
void cellGcmSetFlipStatus2(PPUContext& ctx) { set_flip_status(ctx, 0, 0, kFlipDone); }
void cellGcmResetFlipStatus2(PPUContext& ctx) { set_flip_status(ctx, 0, ~kFlipDone, 0); }

// libgcm_sys's display sync for a head: CELL_GCM_DISPLAY_HSYNC (1) and HSYNC_WITH_NOISE (3) are lv2's 1; VSYNC (2) is
// 3 when lv2 reports the display at 59.94 Hz (sys_rsx_attribute 0x202 answers 1), else 2. lv2's display attribute
// (sys_rsx_attribute 2) first, then 0x101.
s32 display_frequency(PPUContext& ctx, u32 head)
{
    const s32 r = static_cast<s32>(lv2_syscall(ctx, kScRsxAttribute, {0x202, head, 1, 0, 0}));
    return r == 2 ? 2 : r == 3 ? 1 : -1;
}
void set_flip_mode(PPUContext& ctx, u32 mode, u32 head)
{
    u32 attr = 0, sync = 0;
    if (mode == 1) {
        attr = 1;
        sync = 1;
    } else if (mode == 3) {
        attr = 0;
        sync = 1;
    } else {
        const s32 f = display_frequency(ctx, head);
        attr = f == 1 ? 1 : 0;
        sync = f == 1 ? 3 : 2;
    }
    lv2_syscall(ctx, kScRsxAttribute, {2, head, attr, 0, 0});
    rsx_attribute_call(ctx, {context_id(), 0x101, head, sync, 0, 0});
}
void cellGcmSetFlipMode2(PPUContext& ctx, u32 mode) { set_flip_mode(ctx, mode, 0); }

// cellGcmSetVBlankFrequency(freq): package 0x108 for the head's vblank (handler 1): CELL_GCM_DISPLAY_FREQUENCY_59_94HZ
// (1) is lv2's 3, SCANOUT (2) its 2, anything else 4; then the head's display sync becomes VSYNC.
void cellGcmSetVBlankFrequency(PPUContext& ctx, u32 freq)
{
    constexpr u32 head = 1;
    rsx_attribute_call(ctx, {context_id(), 0x108, head, freq == 1 ? 3u : freq == 2 ? 2u : 4u, 1, 0});
    set_flip_mode(ctx, 2, head);
}


// The ring of buffers cellGcmSetPrepareFlip queued: from the slot after `ring_read` up to `ring_write`, the slot
// whose index is `index` gives its buffer (libgcm_sys's lookup); -1 when it is not there. `advance` makes it the last
// flipped one (cellGcmSetFlipImmediate).
s32 ring_lookup(GcmState& g, u32 index, bool advance)
{
    if (index > 7) return -1;
    s32 at = g.ring_read;
    if (at == g.ring_write) return -1;
    for (;;) {
        at = (at + 1) % 8;
        if (static_cast<u32>(at) == index) {
            const s32 id = g.ring[static_cast<u32>(at)];
            if (advance) g.ring_read = at;
            return id;
        }
        if (at == g.ring_write) return -1;
    }
}

s32 cellGcmGetDisplayBufferByFlipIndex(PPUContext&, u32 index)
{
    GcmState& g = gcm();
    std::lock_guard rl(g.ring_mutex);
    return ring_lookup(g, index & 0xFF, false);
}

// libgcm_sys's immediate flip: label 0 or 1 set to 0xFFFFFFFF, then lv2's display flip (0x102) of the head's queued
// buffer `index` with the hardware channel (driver information +0xC) in bits 8..15; CELL_GCM_ERROR_FAILURE before
// cellGcmInit or when lv2 refuses it.
s32 flip_immediate(PPUContext& ctx, u32 index, u32 head)
{
    u8* base = guest_base(ctx);
    GcmState& g = gcm();
    // The RSX reads and writes the labels on its own thread (acquires, a flip's release of label 1): a release store, as
    // the RSX's own (D-587).
    if (g.label_addr) atomic_store_be32(base, g.label_addr + 0x10 * head, 0xFFFFFFFF);
    const u32 di = g.driver_info;
    if (!di) return error(CELL_GCM_ERROR_FAILURE);
    const u64 value = 0x80000000u | (ps_read32(base, di + 0xC) << 8) | (index & 7);
    if (rsx_attribute_call(ctx, {context_id(), 0x102, head, value, 0, 0}) != lv2::CELL_OK) {
        return error(CELL_GCM_ERROR_FAILURE);
    }
    return lv2::CELL_OK;
}

// cellGcmSetFlipImmediate2(index): head 0's queued buffer `index` (0..7).
s32 cellGcmSetFlipImmediate2(PPUContext& ctx, u32 index)
{
    if (index > 7) return error(CELL_GCM_ERROR_FAILURE);
    return flip_immediate(ctx, index & 0xFF, 0);
}

// The flip handler of head 0 (lv2's event 8). Head 0 never flips here (the RSX flips head 1), so it is kept, with its
// event bit, and not called.
void cellGcmSetFlipHandler2(PPUContext& ctx, u32 handler)
{
    gcm().flip_handler_head0.store(handler, std::memory_order_release);
    set_event_mask(ctx, 8, handler != 0);
}

// cellGcmSetSecondVHandler(handler): head 1's second vblank handler (lv2's event 0x800), called on the interrupt
// thread at each vblank once cellGcmSetSecondVFrequency enabled the second vblank.
void cellGcmSetSecondVHandler(PPUContext& ctx, u32 handler)
{
    gcm().second_vhandler[1].store(handler, std::memory_order_release);
    set_event_mask(ctx, 0x800, handler != 0);
}

// _cellGcmSetFlipCommand2(context, id): label 0 set to 0xFFFFFFFF by the RSX (two words of nothing, then
// NV406E_SEMAPHORE_OFFSET 0 and NV406E_SEMAPHORE_RELEASE 0xFFFFFFFF, written without a room check), then
// GCM_FLIP_HEAD for head 0 with id | 0x80000100 (a queued buffer), with libgcm_sys's room check of 16 bytes.
void _cellGcmSetFlipCommand2(PPUContext& ctx, u32 context, u32 id)
{
    if (!context_readable(ctx, context, "_cellGcmSetFlipCommand2")) return;
    u8* base = guest_base(ctx);
    u32 current = ps_read32(base, context + ctx_current);
    const u32 words[6] = {0, 0, 0x00040064, 0, 0x0004006C, 0xFFFFFFFF};
    for (u32 i = 0; i < 6; i++) ps_write32(base, current + 4 * i, words[i]);
    current += 24;
    ps_write32(base, context + ctx_current, current);
    if (current + 16 > ps_read32(base, context + ctx_end)) {
        if (make_room(ctx, base, context) != 0) return;
        current = ps_read32(base, context + ctx_current);
    }
    ps_write32(base, current, rsx::cmd::method_header(rsx::m::GCM_FLIP_HEAD, 1));
    ps_write32(base, current + 4, id | 0x80000100);
    ps_write32(base, context + ctx_current, current + 8);
}

// cellGcmSetInvalidateTile(index): lv2's tile package (0x300) with the tile word cellGcmSetTile last sent for it
// without its location bits (so lv2 unbinds it), then the tile's CellGcmTileInfo cleared. libgcm_sys checks no index
// (it reads and clears past its arrays); an index past 14 reaches lv2 here and clears nothing.
void cellGcmSetInvalidateTile(PPUContext& ctx, u8 index)
{
    GcmState& g = gcm();
    const u32 word = index < kTilesCount ? g.invalidate_tile[index] : 0;
    rsx_attribute_call(ctx, {context_id(), 0x300, index, u64{word} << 32, 0, 0});
    if (index < kTilesCount && g.tiles) std::memset(guest_base(ctx) + g.tiles + 16 * index, 0, 16);
}

// cellGcmDumpGraphicsError: libgcm_sys has nothing to dump (CELL_OK).
s32 cellGcmDumpGraphicsError(PPUContext&) { return lv2::CELL_OK; }

// The hardware cursor, as libgcm_sys keeps it: cellGcmInitCursor (lv2 0x10D), then enable, disable, position (0..4095
// each; a y of 0 is kept as 1), image offset and mode only change what cellGcmUpdateCursor sends lv2: the mode
// (0x10B, 4), the image (0x10B, 2) and the position (0x10B, 3), with the cursor disabled around a change of its
// enabling or mode (0x10C 2, then 1). Before cellGcmInitCursor each is CELL_GCM_ERROR_FAILURE.
s32 cellGcmInitCursor(PPUContext& ctx)
{
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    if (g.cursor.initialized) return error(CELL_GCM_ERROR_FAILURE);
    const u32 id = context_id();
    if (!id) return error(CELL_GCM_ERROR_FAILURE);
    g.cursor = {};
    if (const s32 r = rsx_attribute_call(ctx, {id, 0x10D, 1, 1, 0, 0}); r != lv2::CELL_OK) return r;
    g.cursor.initialized = true;
    return lv2::CELL_OK;
}

s32 set_cursor_enabled(bool on)
{
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    if (!g.cursor.initialized) return error(CELL_GCM_ERROR_FAILURE);
    g.cursor.enable_dirty = true;
    g.cursor.enabled = on;
    return lv2::CELL_OK;
}
s32 cellGcmSetCursorEnable(PPUContext&) { return set_cursor_enabled(true); }
s32 cellGcmSetCursorDisable(PPUContext&) { return set_cursor_enabled(false); }

s32 cellGcmSetCursorPosition(PPUContext&, s32 x, s32 y)
{
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    if (!g.cursor.initialized) return error(CELL_GCM_ERROR_FAILURE);
    if (x > 4095 || y > 4095 || x < 0 || y < 0) return error(CELL_GCM_ERROR_INVALID_VALUE);
    g.cursor.x = static_cast<u32>(x);
    g.cursor.y = y ? static_cast<u32>(y) : 1;
    return lv2::CELL_OK;
}

s32 cellGcmSetCursorImageOffset(PPUContext&, u32 offset)
{
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    if (!g.cursor.initialized || !g.driver_info) return error(CELL_GCM_ERROR_FAILURE);
    g.cursor.offset = offset;
    g.cursor.image_dirty = true;
    return lv2::CELL_OK;
}

s32 cellGcmUpdateCursor(PPUContext& ctx)
{
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    if (!g.cursor.initialized) return error(CELL_GCM_ERROR_FAILURE);
    const u32 id = context_id();
    if (!id) return error(CELL_GCM_ERROR_FAILURE);
    GcmState::Cursor& c = g.cursor;
    const auto send = [&](std::initializer_list<u64> a) { return rsx_attribute_call(ctx, a) == lv2::CELL_OK; };
    if (c.enable_dirty || c.mode_dirty) {
        if (!send({id, 0x10C, 1, 2, 0, 0})) return error(CELL_GCM_ERROR_INVALID_VALUE);
        if (c.mode_dirty) {
            c.mode_dirty = false;
            if (!send({id, 0x10B, 1, 4, static_cast<u64>(static_cast<s64>(static_cast<s32>(c.mode))), 0})) {
                return error(CELL_GCM_ERROR_INVALID_VALUE);
            }
        }
    }
    if (c.image_dirty) {
        c.image_dirty = false;
        if (!send({id, 0x10B, 1, 2, c.offset, 0})) return error(CELL_GCM_ERROR_INVALID_VALUE);
    }
    if (!send({id, 0x10B, 1, 3, c.x, c.y})) return error(CELL_GCM_ERROR_INVALID_VALUE);
    if (!c.enable_dirty) return lv2::CELL_OK;
    c.enable_dirty = false;
    if (!c.enabled) return lv2::CELL_OK;
    return send({id, 0x10C, 1, 1, 0, 0}) ? lv2::CELL_OK : error(CELL_GCM_ERROR_INVALID_VALUE);
}

// GPAD, Sony's GPU capture tool, which a retail system has no part of (libgcm_sys has none of these): no capture is
// running (status 0), and the surface and snapshot requests have nothing to capture them (CELL_OK, as RPCS3).
s32 cellGcmGpadGetStatus(PPUContext& ctx, u32 status)
{
    if (!out_pointer_ok(ctx, status, 4, "cellGcmGpadGetStatus")) return error(CELL_GCM_ERROR_FAILURE);
    ps_write32(guest_base(ctx), status, 0);
    return lv2::CELL_OK;
}
s32 cellGcmGpadNotifyCaptureSurface(PPUContext&, u32) { return lv2::CELL_OK; }
s32 cellGcmGpadCaptureSnapshot(PPUContext&, u32) { return lv2::CELL_OK; }


// cellGcmSortRemapEaIoAddress: libgcm_sys packs the IO mappings together, keeping IO MB 0 (the command buffer) where it
// is: every mapping past it is unmapped in lv2, the offset table's entries are moved down one MB at a time into the
// holes (in IO order, so mappings stay whole), and each mapping is mapped again at its new IO offset (flags
// 0xE000000000000800, as libgcm_sys). The IO space it walks is cellGcmGetMaxIoMapSize's. lv2's error, if one, is the
// answer, the mappings left as they are by then.
s32 cellGcmSortRemapEaIoAddress(PPUContext& ctx)
{
    GcmState& g = gcm();
    std::lock_guard lock(g.mutex);
    const u32 n = max_io_map_size(g) >> 20;
    if (!g.offset_table || !n) return lv2::CELL_OK;
    const u32 id = context_id();
    u8* base = guest_base(ctx);
    const auto ea_at = [&](u32 io) { return u32{ps_read16(base, g.offset_table + 2 * (kIoAddressEntries + io))}; };
    const auto size_at = [&](u32 io) {
        const u32 ea = ea_at(io);
        return ea < kIoAddressEntries ? u32{g.io_map_size[ea]} : 0u;
    };
    for (u32 io = 1; io < n; io++) {
        if (ea_at(io) == 0xFFFF || !size_at(io)) continue;
        if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 673, {id, u64{io} << 20, u64{size_at(io)} << 20}))) return r;
    }
    for (u32 io = 1; io + 1 < n;) {
        if (ea_at(io) != 0xFFFF) {
            io++;
            continue;
        }
        u32 from = io + 1;
        while (from < n && ea_at(from) == 0xFFFF) from++;
        if (from < n) {
            const u32 ea = ea_at(from);
            ps_write16(base, g.offset_table + 2 * ea, static_cast<u16>(io));
            ps_write16(base, g.offset_table + 2 * (kIoAddressEntries + from), 0xFFFF);
            ps_write16(base, g.offset_table + 2 * (kIoAddressEntries + io), static_cast<u16>(ea));
        }
        io++;
    }
    for (u32 io = 1; io < n; io++) {
        if (ea_at(io) == 0xFFFF || !size_at(io)) continue;
        if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 672,
                {id, u64{io} << 20, u64{ea_at(io)} << 20, u64{size_at(io)} << 20, 0xE000000000000800ull}))) {
            return r;
        }
    }
    return lv2::CELL_OK;
}

} // namespace

namespace gcm {

s32 set_display_buffer(PPUContext& ctx, u8 id, u32 offset, u32 pitch, u32 width, u32 height)
{
    return cellGcmSetDisplayBuffer(ctx, id, offset, pitch, width, height);
}
s32 set_flip(PPUContext& ctx, u32 context, u32 id) { return cellGcmSetFlip(ctx, context, id); }
void set_wait_flip(PPUContext& ctx, u32 context) { cellGcmSetWaitFlip(ctx, context); }
void set_flip_handler(PPUContext& ctx, u32 handler) { cellGcmSetFlipHandler(ctx, handler); }
void set_vblank_handler(PPUContext& ctx, u32 handler) { cellGcmSetVBlankHandler(ctx, handler); }
u32 get_flip_status(PPUContext& ctx) { return cellGcmGetFlipStatus(ctx); }
void reset_flip_status(PPUContext& ctx) { cellGcmResetFlipStatus(ctx); }
u64 get_last_flip_time(PPUContext& ctx) { return cellGcmGetLastFlipTime(ctx); }

} // namespace gcm

void register_cellGcmSys(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0x15BAE46B, _cellGcmInitBody),
        PS_HLE_FUNC(0x3A33C1FD, _cellGcmFunc15),
        PS_HLE_FUNC(0x661FE266, _cellGcmFunc12),
        PS_HLE_FUNC(0xE315A0B2, cellGcmGetConfiguration),
        PS_HLE_FUNC(0xA547ADDE, cellGcmGetControlRegister),
        PS_HLE_FUNC(0x5E2EE0F0, cellGcmGetDefaultCommandWordSize),
        PS_HLE_FUNC(0x8CDF8C70, cellGcmGetDefaultSegmentWordSize),
        PS_HLE_FUNC(0xCAABD992, cellGcmInitDefaultFifoMode),
        PS_HLE_FUNC(0x9BA451E4, cellGcmSetDefaultFifoSize),
        PS_HLE_FUNC(0xBC982946, cellGcmSetDefaultCommandBuffer),
        PS_HLE_FUNC(0x172C3197, cellGcmSetDefaultCommandBufferAndSegmentWordSize),
        PS_HLE_FUNC(0xF80196C1, cellGcmGetLabelAddress),
        PS_HLE_FUNC(0x8572BCE2, cellGcmGetReportDataAddressLocation),
        PS_HLE_FUNC(0xA6B180AC, cellGcmGetReportDataLocation),
        PS_HLE_FUNC(0x2AD4951B, cellGcmGetTimeStampLocation),
        PS_HLE_FUNC(0x5A41C10F, cellGcmGetTimeStamp),
        PS_HLE_FUNC(0x99D397AC, cellGcmGetReport),
        PS_HLE_FUNC(0x9A0159AF, cellGcmGetReportDataAddress),
        PS_HLE_FUNC(0x21CEE035, cellGcmGetNotifyDataAddress),
        PS_HLE_FUNC(0x21AC3697, cellGcmAddressToOffset),
        PS_HLE_FUNC(0x2A6FBA9C, cellGcmIoOffsetToAddress),
        PS_HLE_FUNC(0x63441CB4, cellGcmMapEaIoAddress),
        PS_HLE_FUNC(0x626E8518, cellGcmMapEaIoAddressWithFlags),
        PS_HLE_FUNC(0xA114EC67, cellGcmMapMainMemory),
        PS_HLE_FUNC(0xDB769B32, cellGcmMapLocalMemory),
        PS_HLE_FUNC(0xFB81C03E, cellGcmGetMaxIoMapSize),
        PS_HLE_FUNC(0xA7EDE268, cellGcmReserveIoMapSize),
        PS_HLE_FUNC(0x3B9BD5BD, cellGcmUnreserveIoMapSize),
        PS_HLE_FUNC(0xEFD00F54, cellGcmUnmapEaIoAddress),
        PS_HLE_FUNC(0xDB23E867, cellGcmUnmapIoAddress),
        PS_HLE_FUNC(0x2922AED0, cellGcmGetOffsetTable),
        PS_HLE_FUNC(0xA53D12AE, cellGcmSetDisplayBuffer),
        PS_HLE_FUNC(0x0E6B0DAE, cellGcmGetDisplayInfo),
        PS_HLE_FUNC(0x657571F7, cellGcmGetTileInfo),
        PS_HLE_FUNC(0xD9A0A879, cellGcmGetZcullInfo),
        PS_HLE_FUNC(0x72A577CE, cellGcmGetFlipStatus),
        PS_HLE_FUNC(0xB2E761D4, cellGcmResetFlipStatus),
        PS_HLE_FUNC(0xA47C09FF, cellGcmSetFlipStatus),
        PS_HLE_FUNC(0x93806525, cellGcmGetCurrentDisplayBufferId),
        PS_HLE_FUNC(0x63387071, cellGcmGetLastFlipTime),
        PS_HLE_FUNC(0x723BBC7E, cellGcmGetVBlankCount),
        PS_HLE_FUNC(0x21397818, _cellGcmSetFlipCommand),
        PS_HLE_FUNC(0xD8F88E1A, _cellGcmSetFlipCommandWithWaitLabel),
        PS_HLE_FUNC(0x4AE8D215, cellGcmSetFlipMode),
        PS_HLE_FUNC(0x0B4B62D5, cellGcmSetPrepareFlip),
        PS_HLE_FUNC(0xDC09357E, cellGcmSetFlip),
        PS_HLE_FUNC(0x983FB9AA, cellGcmSetWaitFlip),
        PS_HLE_FUNC(0xDF6476BD, cellGcmSetWaitFlipUnsafe),
        PS_HLE_FUNC(0x8BDE5EBF, cellGcmSetUserCommand),
        PS_HLE_FUNC(0xACEE8542, cellGcmSetFlipImmediate),
        PS_HLE_FUNC(0x4D7CE993, cellGcmSetSecondVFrequency),
        PS_HLE_FUNC(0x51C9D62B, cellGcmSetDebugOutputLevel),
        PS_HLE_FUNC(0x527C6439, cellGcmTerminate),
        PS_HLE_FUNC(0xA91B0402, cellGcmSetVBlankHandler),
        PS_HLE_FUNC(0xD01B570D, cellGcmSetGraphicsHandler),
        PS_HLE_FUNC(0xA41EF7E8, cellGcmSetFlipHandler),
        PS_HLE_FUNC(0x06EDEA9E, cellGcmSetUserHandler),
        PS_HLE_FUNC(0x0A862772, cellGcmSetQueueHandler),
        PS_HLE_FUNC(0xFCE9E764, cellGcmInitSystemMode),
        PS_HLE_FUNC(0xBD100DBC, cellGcmSetTileInfo),
        PS_HLE_FUNC(0xD0B1D189, cellGcmSetTile),
        PS_HLE_FUNC(0x4524CCCD, cellGcmBindTile),
        PS_HLE_FUNC(0xD9B7653E, cellGcmUnbindTile),
        PS_HLE_FUNC(0xD34A420D, cellGcmSetZcull),
        PS_HLE_FUNC(0x9DC04436, cellGcmBindZcull),
        PS_HLE_FUNC(0xA75640E8, cellGcmUnbindZcull),
        PS_HLE_FUNC(0x055BD74D, cellGcmGetTiledPitchSize),
        PS_HLE_INTERNAL(NID_cellGcmCallback, cellGcmCallback),
        PS_HLE_INTERNAL(NID_interrupt_thread, _pspocket_gcm_interrupt_thread),
    });
    using detail::thunk;
    const auto f = [](std::string_view name, HleFn fn) { return Function{{}, name, recomp::nid_of(name), fn, false}; };
    r.add(kModule, {
        f("_cellGcmFunc1", &thunk<&_cellGcmFunc1>), f("_cellGcmFunc2", &thunk<&_cellGcmFunc2>),
        f("_cellGcmFunc3", &thunk<&_cellGcmFunc3>), f("_cellGcmFunc4", &thunk<&_cellGcmFunc4>),
        f("_cellGcmFunc13", &thunk<&_cellGcmFunc13>), f("_cellGcmFunc38", &thunk<&_cellGcmFunc38>),
        f("_cellGcmSetFlipCommand2", &thunk<&_cellGcmSetFlipCommand2>),
        f("cellGcmDumpGraphicsError", &thunk<&cellGcmDumpGraphicsError>),
        f("cellGcmGetCurrentField", &thunk<&cellGcmGetCurrentField>),
        f("cellGcmGetDisplayBufferByFlipIndex", &thunk<&cellGcmGetDisplayBufferByFlipIndex>),
        f("cellGcmGetFlipStatus2", &thunk<&cellGcmGetFlipStatus2>),
        f("cellGcmGetLastFlipTime2", &thunk<&cellGcmGetLastFlipTime2>),
        f("cellGcmGetLastSecondVTime", &thunk<&cellGcmGetLastSecondVTime>),
        f("cellGcmGetVBlankCount2", &thunk<&cellGcmGetVBlankCount2>),
        f("cellGcmGpadCaptureSnapshot", &thunk<&cellGcmGpadCaptureSnapshot>),
        f("cellGcmGpadGetStatus", &thunk<&cellGcmGpadGetStatus>),
        f("cellGcmGpadNotifyCaptureSurface", &thunk<&cellGcmGpadNotifyCaptureSurface>),
        f("cellGcmInitCursor", &thunk<&cellGcmInitCursor>),
        f("cellGcmResetFlipStatus2", &thunk<&cellGcmResetFlipStatus2>),
        f("cellGcmSetCursorDisable", &thunk<&cellGcmSetCursorDisable>),
        f("cellGcmSetCursorEnable", &thunk<&cellGcmSetCursorEnable>),
        f("cellGcmSetCursorImageOffset", &thunk<&cellGcmSetCursorImageOffset>),
        f("cellGcmSetCursorPosition", &thunk<&cellGcmSetCursorPosition>),
        f("cellGcmSetFlipHandler2", &thunk<&cellGcmSetFlipHandler2>),
        f("cellGcmSetFlipImmediate2", &thunk<&cellGcmSetFlipImmediate2>),
        f("cellGcmSetFlipMode2", &thunk<&cellGcmSetFlipMode2>),
        f("cellGcmSetFlipStatus2", &thunk<&cellGcmSetFlipStatus2>),
        f("cellGcmSetInvalidateTile", &thunk<&cellGcmSetInvalidateTile>),
        f("cellGcmSetSecondVHandler", &thunk<&cellGcmSetSecondVHandler>),
        f("cellGcmSetVBlankFrequency", &thunk<&cellGcmSetVBlankFrequency>),
        f("cellGcmSysGetLastVBlankTime", &thunk<&cellGcmSysGetLastVBlankTime>),
        f("cellGcmUpdateCursor", &thunk<&cellGcmUpdateCursor>),
        f("cellGcmSortRemapEaIoAddress", &thunk<&cellGcmSortRemapEaIoAddress>),
    });
}

// ---- the screen's refresh (D-971) ----------------------------------------------------------------------------------

namespace {
std::atomic<s64> g_vsync_ns{0};        // the last reported refresh, steady_clock nanoseconds (0: none)
std::atomic<s64> g_vsync_period_ns{0}; // the time from one refresh to the next
} // namespace

void display_vsync(std::chrono::steady_clock::time_point at, std::chrono::nanoseconds period)
{
    if (period.count() <= 0) return;
    g_vsync_period_ns.store(period.count(), std::memory_order_relaxed);
    g_vsync_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count(),
        std::memory_order_release);
}

void display_vsync_reset()
{
    g_vsync_ns.store(0, std::memory_order_release);
    g_vsync_period_ns.store(0, std::memory_order_relaxed);
}

std::chrono::steady_clock::time_point next_vblank(std::chrono::steady_clock::time_point previous,
    std::chrono::steady_clock::time_point nominal, std::chrono::steady_clock::time_point now)
{
    using std::chrono::nanoseconds;
    const s64 last = g_vsync_ns.load(std::memory_order_acquire);
    const s64 period = g_vsync_period_ns.load(std::memory_order_relaxed);
    const auto ns = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<nanoseconds>(t.time_since_epoch()).count();
    };
    if (last == 0 || period <= 0 || ns(now) - last > 100'000'000) return nominal;
    // Refreshes fall at last + k * period, k of either sign: the one nearest the nominal time.
    const s64 d = ns(nominal) - last;
    const s64 k = d >= 0 ? (d + period / 2) / period : -((-d + period / 2) / period);
    s64 t = last + k * period;
    const s64 earliest = ns(previous) + (ns(nominal) - ns(previous)) / 2;
    while (t < earliest) t += period;
    using Clock = std::chrono::steady_clock;
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(nanoseconds(t)));
}

} // namespace pspocket::hle
