// cellDmux: the demultiplexer games use for their movies (D-194), pspocket's own over its PAMF core (cellDmuxPamf.cpp,
// dmux.hpp). The game opens a demuxer in memory it provides, enables the elementary streams it wants (a video, an
// audio track, subtitles), gives it a stream, and gets each access unit through a callback and an access unit queue
// it reads with cellDmuxGetAu and gives back with cellDmuxReleaseAu.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellDmux.cpp/.h, which reproduces libdmux: the contexts in the game's memory
// (DmuxContext, DmuxEsContext and the access unit queue after it), their lv2 mutexes, every check, the sizes asked for,
// the core's errors mapped to the API's, the queue and its error recovery. pspocket keeps all of it; the core is called
// directly instead of through CellDmuxCoreOps.
//
// Differences from RPCS3, each measured against Sony's libdmux (tests/hle/test_dmux.cpp, D-194):
// - QueryAttr's size has no room for an elementary stream context: align16(0xC0 + 4 * streams) + the core's + 0xF
//   (RPCS3 adds sizeof(DmuxEsContext), 0x50 more than Sony's, so a game giving exactly Sony's size was refused).
// - Pointers RPCS3 dereferences without a check (it would fault, as LLE) are checked; one that is not guest memory is
//   logged and refused with CELL_DMUX_ERROR_ARG (D-042).

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

#include "dmux.hpp"
#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellDmux";

enum : u32 {
    CELL_DMUX_ERROR_ARG = 0x80610201,
    CELL_DMUX_ERROR_SEQ = 0x80610202,
    CELL_DMUX_ERROR_BUSY = 0x80610203,
    CELL_DMUX_ERROR_EMPTY = 0x80610204,
    CELL_DMUX_ERROR_FATAL = 0x80610205,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr s32 CELL_DMUX_STREAM_TYPE_PAMF = 1;
constexpr s32 CELL_DMUX_MSG_TYPE_DEMUX_DONE = 0, CELL_DMUX_MSG_TYPE_FATAL_ERR = 1, CELL_DMUX_MSG_TYPE_PROG_END_CODE = 2;
constexpr s32 CELL_DMUX_ES_MSG_TYPE_AU_FOUND = 0, CELL_DMUX_ES_MSG_TYPE_FLUSH_DONE = 1;
constexpr u32 DMUX_STOPPED = 1, DMUX_RUNNING = 2;

// DmuxContext (0xc0, aligned 0x10), the CellDmuxHandle; the enabled streams' handles follow it.
namespace dx {
constexpr u32 kSize = 0xc0, kThis = 0x00, kThisSize = 0x04, kVersion = 0x08, kState = 0x0c, kType = 0x10,
              kCbFunc = 0x1c, kCbArg = 0x20, kStreamIsSet = 0x24, kCoreHandle = 0x28, kVersion2 = 0x2c,
              kUserData = 0x30, kMaxEs = 0x38, kEnabledEs = 0x3c, kMutex = 0x40;
} // namespace dx

// DmuxEsContext (0x50, aligned 0x10), the CellDmuxEsHandle; its access unit queue's elements follow it.
namespace es {
constexpr u32 kSize = 0x50, kMutex = 0x00, kIsEnabled = 0x04, kErrorMemSize = 0x08, kErrorCount = 0x0c,
              kErrorMemAddr = 0x10, kThis = 0x14, kThisSize = 0x18, kThisIndex = 0x1c, kDmuxHandle = 0x20,
              kCbFunc = 0x24, kCbArg = 0x28, kCoreEsHandle = 0x2c, kFlushStarted = 0x30, kQMax = 0x34,
              kQAllocated = 0x38, kQSize = 0x3c, kQFront = 0x40, kQBack = 0x44, kQAllocatedBack = 0x48;
// DmuxAuQueueElement (0x38): index, an unused byte, then DmuxAuInfo: CellDmuxAuInfo (auAddr, auSize, auMaxSize, isRap,
// userData, pts, dts), the specific information's address and size.
constexpr u32 kElem = 0x38, kElIndex = 0x00, kElUnk = 0x04, kElInfo = 0x08, kElAuAddr = 0x08, kElAuSize = 0x0c,
              kElAuMaxSize = 0x10, kElIsRap = 0x14, kElUserData = 0x18, kElPts = 0x20, kElDts = 0x28,
              kElSpecific = 0x30, kElSpecificSize = 0x34;
} // namespace es

u32 align16(u32 v) { return (v + 0xf) & ~0xfu; }

s32 finish(s32 r, const std::string& call)
{
    if (r < 0) PS_LOG_WARN(kChan, "{}: {:#010x}", call, static_cast<u32>(r));
    else PS_LOG_DEBUG(kChan, "{}: CELL_OK", call);
    return r;
}

bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (addr && guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("cellDmux: {} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// RPCS3 get_error: the core's CellDmuxPamfError to the API's (2 to 5 are all ARG).
s32 get_error(u32 internal)
{
    switch (internal) {
    case 0: return lv2::CELL_OK;
    case 2:
    case 3:
    case 4:
    case 5: return error(CELL_DMUX_ERROR_ARG);
    default: return error(CELL_DMUX_ERROR_FATAL);
    }
}

s32 mutex_lock(PPUContext& ctx, u32 id) { return lv2::sys_mutex_lock(thread_of(ctx), id, 0); }
s32 mutex_unlock(PPUContext& ctx, u32 id) { return lv2::sys_mutex_unlock(thread_of(ctx), id); }

// Unlocks and answers `ret`, or the unlock's own error.
s32 unlock_with(PPUContext& ctx, u32 mutex, s32 ret)
{
    const s32 r = mutex_unlock(ctx, mutex);
    return r != lv2::CELL_OK ? r : ret;
}

// An lv2 mutex for the API (priority, not recursive), named as libdmux names it.
s32 create_mutex(PPUContext& ctx, u32 id_out, std::string_view name)
{
    u8* base = guest_base(ctx);
    StackVar attr(ctx, base, 0x28);
    ps_write32(base, attr.addr() + 0x00, lv2::SYS_SYNC_PRIORITY);
    ps_write32(base, attr.addr() + 0x04, lv2::SYS_SYNC_NOT_RECURSIVE);
    ps_write32(base, attr.addr() + 0x08, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    ps_write32(base, attr.addr() + 0x0C, lv2::SYS_SYNC_NOT_ADAPTIVE);
    for (u32 i = 0; i < 8; i++) ps_write8(base, attr.addr() + 0x20 + i, i < name.size() ? static_cast<u8>(name[i]) : 0);
    return lv2::sys_mutex_create(thread_of(ctx), id_out, attr.addr());
}

// The game's demuxer callback: (demuxerHandle, CellDmuxMsg*, cbArg).
void dmux_callback(PPUContext& ctx, u32 handle, s32 msg_type, u64 supplemental)
{
    u8* base = guest_base(ctx);
    StackVar msg(ctx, base, 0x10);
    ps_write32(base, msg.addr(), static_cast<u32>(msg_type));
    ps_write64(base, msg.addr() + 8, supplemental);
    call_guest(ctx, ps_read32(base, handle + dx::kCbFunc), {handle, msg.addr(), ps_read32(base, handle + dx::kCbArg)});
}

// The game's elementary stream callback: (demuxerHandle, esHandle, CellDmuxEsMsg*, cbArg).
void es_callback(PPUContext& ctx, u32 es_handle, s32 msg_type, u64 supplemental)
{
    u8* base = guest_base(ctx);
    const u32 dmux = ps_read32(base, es_handle + es::kDmuxHandle);
    StackVar msg(ctx, base, 0x10);
    ps_write32(base, msg.addr(), static_cast<u32>(msg_type));
    ps_write64(base, msg.addr() + 8, supplemental);
    call_guest(ctx, ps_read32(base, es_handle + es::kCbFunc),
        {dmux, es_handle, msg.addr(), ps_read32(base, es_handle + es::kCbArg)});
}

bool pamf_type(u8* base, u32 handle) { return static_cast<s32>(ps_read32(base, handle + dx::kType)) == CELL_DMUX_STREAM_TYPE_PAMF; }

// A demuxer handle the game passes: guest memory, open (_this), of the PAMF type.
bool live_handle(PPUContext& ctx, u32 handle, std::string_view call)
{
    if (!handle) return false;
    if (!usable(ctx, handle, dx::kSize, call)) return false;
    u8* base = guest_base(ctx);
    return ps_read32(base, handle + dx::kThis) != 0 && pamf_type(base, handle);
}

// An elementary stream handle the game passes: guest memory, enabled (_this), of a PAMF demuxer.
bool live_es(PPUContext& ctx, u32 es_handle, std::string_view call)
{
    if (!es_handle || !usable(ctx, es_handle, es::kSize, call)) return false;
    u8* base = guest_base(ctx);
    const u32 dmux = ps_read32(base, es_handle + es::kDmuxHandle);
    return ps_read32(base, es_handle + es::kThis) != 0 && dmux && usable(ctx, dmux, dx::kSize, call) && pamf_type(base, dmux);
}

u32 elements(u32 es_handle) { return es_handle + es::kSize; }

// RPCS3 query_attr: the API's size from the core's, as Sony's libdmux computes it (see the top).
s32 query_attr(PPUContext& ctx, u32 attr, u32 specific_info)
{
    dmux::PamfAttr pamf;
    if (const s32 r = get_error(dmux::core_query_attr(ctx, specific_info, pamf)); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    ps_write32(base, attr + 0, align16(dx::kSize + pamf.max_enabled_es_num * 4) + pamf.mem_size + 0xf);
    ps_write32(base, attr + 4, 0x260000);      // demuxerVerUpper
    ps_write32(base, attr + 8, pamf.version);  // demuxerVerLower
    return lv2::CELL_OK;
}

// RPCS3 open: the core opened in the memory after the context and the stream handles, then the context.
s32 open(PPUContext& ctx, s32 stream_type, const dmux::CoreResource& resource, const dmux::CoreResourceSpurs* spurs,
    u32 cb, u32 specific_info, u32 handle_out)
{
    u8* base = guest_base(ctx);
    StackVar attr(ctx, base, 0x10);
    if (const s32 r = query_attr(ctx, attr.addr(), specific_info); r != lv2::CELL_OK) return r;
    if (ps_read32(base, attr.addr()) > resource.mem_size) return error(CELL_DMUX_ERROR_ARG);
    dmux::PamfAttr core_attr;
    if (const s32 r = get_error(dmux::core_query_attr(ctx, specific_info, core_attr)); r != lv2::CELL_OK) return r;
    const u32 handle = align16(resource.mem_addr);
    const u32 es_handles_size = core_attr.max_enabled_es_num * 4;
    const u32 core_mem = align16(handle + dx::kSize + es_handles_size);
    if (!usable(ctx, resource.mem_addr, resource.mem_size, "the demuxer's memory")) return error(CELL_DMUX_ERROR_ARG);
    dmux::CoreResource core_res = resource;
    core_res.mem_addr = core_mem;
    core_res.mem_size = resource.mem_size - (core_mem - resource.mem_addr);
    u32 core_handle = 0;
    if (const s32 r = get_error(dmux::core_open(ctx, specific_info, core_res, spurs, handle, core_handle)); r != lv2::CELL_OK)
        return r;
    ps_write32(base, handle + dx::kThis, handle);
    ps_write32(base, handle + dx::kThisSize, dx::kSize + es_handles_size);
    ps_write32(base, handle + dx::kVersion, core_attr.version);
    ps_write32(base, handle + dx::kState, DMUX_STOPPED);
    ps_write32(base, handle + dx::kType, static_cast<u32>(stream_type));
    ps_write32(base, handle + dx::kType + 4, 0);
    ps_write32(base, handle + dx::kType + 8, 0);
    ps_write32(base, handle + dx::kCbFunc, ps_read32(base, cb));
    ps_write32(base, handle + dx::kCbArg, ps_read32(base, cb + 4));
    ps_write8(base, handle + dx::kStreamIsSet, 0);
    ps_write32(base, handle + dx::kCoreHandle, core_handle);
    ps_write32(base, handle + dx::kVersion2, core_attr.version);
    ps_write64(base, handle + dx::kUserData, 0);
    ps_write32(base, handle + dx::kMaxEs, core_attr.max_enabled_es_num);
    ps_write32(base, handle + dx::kEnabledEs, 0);
    if (const s32 r = create_mutex(ctx, handle + dx::kMutex, "_dx_mhd"); r != lv2::CELL_OK) return r;
    ps_write32(base, handle_out, handle);
    return lv2::CELL_OK;
}

dmux::CoreResource read_resource(u8* base, u32 r)
{
    return {ps_read32(base, r), ps_read32(base, r + 4), ps_read32(base, r + 8), ps_read32(base, r + 12),
        ps_read32(base, r + 16), ps_read32(base, r + 20)};
}

// CellDmuxResourceEx (0x20): memAddr, memSize, ppuThreadPriority, ppuThreadStackSize, spurs, priority (u64 at 0x14,
// unaligned), maxContention.
bool resource_ex_ok(u8* base, u32 r)
{
    return ps_read32(base, r) && ps_read32(base, r + 4) != 0xFFFFFFFF && ps_read32(base, r + 12) != 0xFFFFFFFF &&
           ps_read32(base, r + 16) && ps_read32(base, r + 0x1c) != 0 && (ps_read64(base, r + 0x14) & 0xf0f0f0f0f0f0f0f0ull) == 0;
}

s32 open_ex(PPUContext& ctx, u32 type, u32 resource_ex, u32 cb, u32 specific_info, u32 handle_out)
{
    u8* base = guest_base(ctx);
    const dmux::CoreResource res{ps_read32(base, resource_ex), ps_read32(base, resource_ex + 4),
        ps_read32(base, resource_ex + 8), ps_read32(base, resource_ex + 12), 0xfa, 1};
    const dmux::CoreResourceSpurs spurs{ps_read32(base, resource_ex + 16), ps_read64(base, resource_ex + 0x14),
        ps_read32(base, resource_ex + 0x1c)};
    return open(ctx, type, res, &spurs, cb, specific_info, handle_out);
}

// RPCS3 disable_es: the core lets the stream go, then its mutex (retried while a callback holds it).
s32 disable_es(PPUContext& ctx, u32 es_handle)
{
    u8* base = guest_base(ctx);
    const u32 mes = ps_read32(base, es_handle + es::kMutex);
    if (const s32 r = mutex_lock(ctx, mes); r != lv2::CELL_OK) return r;
    const u32 core_ret = dmux::core_disable_es(ctx, ps_read32(base, es_handle + es::kCoreEsHandle));
    ps_write32(base, es_handle + es::kIsEnabled, 0);
    if (const s32 r = mutex_unlock(ctx, mes); r != lv2::CELL_OK) return r;
    s32 r = 0;
    while ((r = lv2::sys_mutex_destroy(thread_of(ctx), mes)) == lv2::err(lv2::CELL_EBUSY))
        lv2::sys_timer_usleep(thread_of(ctx), 200);
    if (r != lv2::CELL_OK) return r;
    ps_write32(base, es_handle + es::kThis, 0);
    return get_error(core_ret);
}

// RPCS3 pop_au: the front of the queue, taken or only looked at.
s32 pop_au(PPUContext& ctx, u32 es_handle, u32 au_info_out, u32 specific_out, bool peek, const std::string& call)
{
    if (!live_es(ctx, es_handle, call)) return error(CELL_DMUX_ERROR_ARG);
    if ((au_info_out && !usable(ctx, au_info_out, 4, call)) || (specific_out && !usable(ctx, specific_out, 4, call)))
        return error(CELL_DMUX_ERROR_ARG);
    u8* base = guest_base(ctx);
    const u32 mes = ps_read32(base, es_handle + es::kMutex);
    if (const s32 r = mutex_lock(ctx, mes); r != lv2::CELL_OK) return r;
    if (static_cast<s32>(ps_read32(base, es_handle + es::kQSize)) <= 0) return unlock_with(ctx, mes, error(CELL_DMUX_ERROR_EMPTY));
    const u32 front = ps_read32(base, es_handle + es::kQFront);
    const u32 element = elements(es_handle) + front * es::kElem;
    if (au_info_out) ps_write32(base, au_info_out, element + es::kElInfo);
    if (specific_out) ps_write32(base, specific_out, ps_read32(base, element + es::kElSpecific));
    if (!peek) {
        ps_write32(base, es_handle + es::kQFront, (front + 1) % ps_read32(base, es_handle + es::kQMax));
        ps_write32(base, es_handle + es::kQSize, ps_read32(base, es_handle + es::kQSize) - 1);
    }
    return mutex_unlock(ctx, mes);
}

} // namespace

// ---- the core's notifications (dmux.hpp), on its PPU thread

namespace dmux {

void notify_demux_done(PPUContext& ctx, u32 handle)
{
    u8* base = guest_base(ctx);
    const u32 mhd = ps_read32(base, handle + dx::kMutex);
    if (mutex_lock(ctx, mhd) != lv2::CELL_OK) PS_LOG_ERROR(kChan, "notify_demux_done: _dx_mhd could not be locked");
    ps_write32(base, handle + dx::kState, DMUX_STOPPED);
    if (mutex_unlock(ctx, mhd) != lv2::CELL_OK) PS_LOG_ERROR(kChan, "notify_demux_done: _dx_mhd could not be unlocked");
    if (ps_read32(base, handle + dx::kThis))
        dmux_callback(ctx, handle, CELL_DMUX_MSG_TYPE_DEMUX_DONE, ps_read64(base, handle + dx::kUserData));
}

void notify_fatal_err(PPUContext& ctx, u32 err, u32 handle)
{
    PS_LOG_ERROR(kChan, "notify_fatal_err(error={}, handle={:#x})", err, handle);
    dmux_callback(ctx, handle, CELL_DMUX_MSG_TYPE_FATAL_ERR, static_cast<u32>(get_error(err)));
}

void notify_prog_end_code(PPUContext& ctx, u32 handle)
{
    u8* base = guest_base(ctx);
    if (ps_read32(base, handle + dx::kThis))
        dmux_callback(ctx, handle, CELL_DMUX_MSG_TYPE_PROG_END_CODE, ps_read64(base, handle + dx::kUserData));
}

u32 notify_es_au_found(PPUContext& ctx, const AuInfo& au, u32 es_handle)
{
    u8* base = guest_base(ctx);
    const u32 mes = ps_read32(base, es_handle + es::kMutex);
    const auto enabled = [&] { return ps_read32(base, es_handle + es::kIsEnabled) != 0; };
    const auto fatal_err = [&](s32 ret) {
        if (enabled())
            dmux_callback(ctx, ps_read32(base, es_handle + es::kDmuxHandle), CELL_DMUX_MSG_TYPE_FATAL_ERR,
                static_cast<u64>(static_cast<u32>(ret)));
    };
    // The stream may be disabled at any time by another thread (cellDmuxDisableEs, cellDmuxClose): checked often.
    if (!enabled()) return 0;
    if (const s32 r = mutex_lock(ctx, mes); r != lv2::CELL_OK) {
        fatal_err(r);
        return 1;
    }
    // Full? One slot is kept for the access unit a flush makes, so that flushing always succeeds.
    const s32 max_size = static_cast<s32>(ps_read32(base, es_handle + es::kQMax));
    const s32 flush_started = static_cast<s32>(ps_read32(base, es_handle + es::kFlushStarted) & 1);
    if (!enabled() || static_cast<s32>(ps_read32(base, es_handle + es::kQAllocated)) >= max_size - !flush_started) {
        if (const s32 r = mutex_unlock(ctx, mes); r != lv2::CELL_OK) {
            fatal_err(r);
            return 1;
        }
        return enabled() ? 1 : 0; // a full queue is expected often: not reported
    }
    const u32 element = elements(es_handle) + ps_read32(base, es_handle + es::kQBack) * es::kElem;
    if (const s32 r = mutex_unlock(ctx, mes); r != lv2::CELL_OK) {
        fatal_err(r);
        return 1;
    }
    ps_write32(base, element + es::kElAuAddr, au.addr);
    ps_write32(base, element + es::kElAuSize, au.size);
    ps_write32(base, element + es::kElAuMaxSize, 0); // left uninitialised by libdmuxpamf (RPCS3)
    ps_write8(base, element + es::kElIsRap, au.is_rap);
    ps_write64(base, element + es::kElUserData, au.user_data);
    ps_write64(base, element + es::kElPts, au.pts);
    ps_write64(base, element + es::kElDts, au.dts);
    const u32 specific = ps_read32(base, element + es::kElSpecific);
    for (u32 i = 0; i < au.specific_info_size && i < au.specific_info.size(); i++) ps_write8(base, specific + i, au.specific_info[i]);
    if (!enabled()) return 0;
    if (const s32 r = mutex_lock(ctx, mes); r != lv2::CELL_OK) {
        fatal_err(r);
        return 0; // CELL_OK on LLE
    }
    if (!enabled()) {
        if (const s32 r = mutex_unlock(ctx, mes); r != lv2::CELL_OK) fatal_err(r);
        return 0;
    }
    ps_write32(base, es_handle + es::kQBack, (ps_read32(base, es_handle + es::kQBack) + 1) % static_cast<u32>(max_size));
    ps_write32(base, es_handle + es::kQAllocated, ps_read32(base, es_handle + es::kQAllocated) + 1);
    ps_write32(base, es_handle + es::kQSize, ps_read32(base, es_handle + es::kQSize) + 1);
    if (const s32 r = mutex_unlock(ctx, mes); r != lv2::CELL_OK) {
        fatal_err(r);
        return 0;
    }
    if (!enabled()) return 0;
    es_callback(ctx, es_handle, CELL_DMUX_ES_MSG_TYPE_AU_FOUND,
        ps_read64(base, ps_read32(base, es_handle + es::kDmuxHandle) + dx::kUserData));
    return 0;
}

void notify_es_flush_done(PPUContext& ctx, u32 es_handle)
{
    u8* base = guest_base(ctx);
    const u32 dmux = ps_read32(base, es_handle + es::kDmuxHandle);
    if (!dmux || !ps_read32(base, dmux + dx::kThis) || !ps_read32(base, es_handle + es::kIsEnabled)) return;
    ps_write32(base, es_handle + es::kFlushStarted, ps_read32(base, es_handle + es::kFlushStarted) & ~1u);
    es_callback(ctx, es_handle, CELL_DMUX_ES_MSG_TYPE_FLUSH_DONE, ps_read64(base, dmux + dx::kUserData));
}

} // namespace dmux

// ---- the API

s32 cellDmuxQueryAttr(PPUContext& ctx, u32 demuxerType, u32 demuxerAttr)
{
    const std::string call = std::format("cellDmuxQueryAttr({:#x}, {:#x})", demuxerType, demuxerAttr);
    if (!demuxerType || !demuxerAttr || !usable(ctx, demuxerType, 12, call) || !usable(ctx, demuxerAttr, 12, call) ||
        static_cast<s32>(ps_read32(guest_base(ctx), demuxerType)) != CELL_DMUX_STREAM_TYPE_PAMF)
        return finish(error(CELL_DMUX_ERROR_ARG), call);
    return finish(query_attr(ctx, demuxerAttr, 0), call);
}

s32 cellDmuxQueryAttr2(PPUContext& ctx, u32 demuxerType2, u32 demuxerAttr)
{
    const std::string call = std::format("cellDmuxQueryAttr2({:#x}, {:#x})", demuxerType2, demuxerAttr);
    if (!demuxerType2 || !demuxerAttr || !usable(ctx, demuxerType2, 8, call) || !usable(ctx, demuxerAttr, 12, call) ||
        static_cast<s32>(ps_read32(guest_base(ctx), demuxerType2)) != CELL_DMUX_STREAM_TYPE_PAMF)
        return finish(error(CELL_DMUX_ERROR_ARG), call);
    return finish(query_attr(ctx, demuxerAttr, ps_read32(guest_base(ctx), demuxerType2 + 4)), call);
}

s32 cellDmuxOpen(PPUContext& ctx, u32 demuxerType, u32 demuxerResource, u32 demuxerCb, u32 demuxerHandle)
{
    const std::string call = std::format("cellDmuxOpen({:#x}, {:#x}, {:#x}, {:#x})", demuxerType, demuxerResource,
        demuxerCb, demuxerHandle);
    u8* base = guest_base(ctx);
    if (!demuxerType || !usable(ctx, demuxerType, 12, call) ||
        static_cast<s32>(ps_read32(base, demuxerType)) != CELL_DMUX_STREAM_TYPE_PAMF || !demuxerResource ||
        !usable(ctx, demuxerResource, 0x18, call) || !ps_read32(base, demuxerResource) ||
        ps_read32(base, demuxerResource + 4) == 0xFFFFFFFF || ps_read32(base, demuxerResource + 12) == 0xFFFFFFFF ||
        !demuxerCb || !usable(ctx, demuxerCb, 8, call) || !ps_read32(base, demuxerCb) || !demuxerHandle ||
        !usable(ctx, demuxerHandle, 4, call))
        return finish(error(CELL_DMUX_ERROR_ARG), call);
    return finish(open(ctx, CELL_DMUX_STREAM_TYPE_PAMF, read_resource(base, demuxerResource), nullptr, demuxerCb, 0,
                      demuxerHandle),
        call);
}

s32 cellDmuxOpenEx(PPUContext& ctx, u32 demuxerType, u32 demuxerResourceEx, u32 demuxerCb, u32 demuxerHandle)
{
    const std::string call = std::format("cellDmuxOpenEx({:#x}, {:#x}, {:#x}, {:#x})", demuxerType, demuxerResourceEx,
        demuxerCb, demuxerHandle);
    u8* base = guest_base(ctx);
    if (!demuxerType || !usable(ctx, demuxerType, 12, call) ||
        static_cast<s32>(ps_read32(base, demuxerType)) != CELL_DMUX_STREAM_TYPE_PAMF || !demuxerResourceEx ||
        !usable(ctx, demuxerResourceEx, 0x20, call) || !resource_ex_ok(base, demuxerResourceEx) || !demuxerCb ||
        !usable(ctx, demuxerCb, 8, call) || !demuxerHandle || !usable(ctx, demuxerHandle, 4, call))
        return finish(error(CELL_DMUX_ERROR_ARG), call);
    return finish(open_ex(ctx, CELL_DMUX_STREAM_TYPE_PAMF, demuxerResourceEx, demuxerCb, 0, demuxerHandle), call);
}

s32 cellDmuxOpenExt(PPUContext& ctx, u32 demuxerType, u32 demuxerResourceEx, u32 demuxerCb, u32 demuxerHandle)
{
    return cellDmuxOpenEx(ctx, demuxerType, demuxerResourceEx, demuxerCb, demuxerHandle);
}

s32 cellDmuxOpen2(PPUContext& ctx, u32 demuxerType2, u32 demuxerResource2, u32 demuxerCb, u32 demuxerHandle)
{
    const std::string call = std::format("cellDmuxOpen2({:#x}, {:#x}, {:#x}, {:#x})", demuxerType2, demuxerResource2,
        demuxerCb, demuxerHandle);
    u8* base = guest_base(ctx);
    if (!demuxerType2 || !usable(ctx, demuxerType2, 8, call) ||
        static_cast<s32>(ps_read32(base, demuxerType2)) != CELL_DMUX_STREAM_TYPE_PAMF || !demuxerResource2 ||
        !usable(ctx, demuxerResource2, 0x24, call) || !demuxerCb || !usable(ctx, demuxerCb, 8, call) ||
        !ps_read32(base, demuxerCb) || !demuxerHandle || !usable(ctx, demuxerHandle, 4, call))
        return finish(error(CELL_DMUX_ERROR_ARG), call);
    const u32 specific = ps_read32(base, demuxerType2 + 4);
    const u32 res = demuxerResource2 + 4; // CellDmuxResource2: isResourceEx, then the union
    if (ps_read8(base, demuxerResource2)) {
        if (!resource_ex_ok(base, res)) return finish(error(CELL_DMUX_ERROR_ARG), call);
        return finish(open_ex(ctx, CELL_DMUX_STREAM_TYPE_PAMF, res, demuxerCb, specific, demuxerHandle), call);
    }
    if (!ps_read32(base, res) || ps_read32(base, res + 4) == 0xFFFFFFFF || ps_read32(base, res + 12) == 0xFFFFFFFF)
        return finish(error(CELL_DMUX_ERROR_ARG), call);
    return finish(open(ctx, CELL_DMUX_STREAM_TYPE_PAMF, read_resource(base, res), nullptr, demuxerCb, specific,
                      demuxerHandle),
        call);
}

s32 cellDmuxClose(PPUContext& ctx, u32 demuxerHandle)
{
    const std::string call = std::format("cellDmuxClose({:#x})", demuxerHandle);
    if (!live_handle(ctx, demuxerHandle, call)) return finish(error(CELL_DMUX_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    const u32 mhd = ps_read32(base, demuxerHandle + dx::kMutex);
    ps_write32(base, demuxerHandle + dx::kThis, 0);
    const auto restore = [&] { ps_write32(base, demuxerHandle + dx::kThis, demuxerHandle); };
    if (const s32 r = mutex_lock(ctx, mhd); r != lv2::CELL_OK) {
        restore();
        return finish(r, call);
    }
    const s32 n = static_cast<s32>(ps_read32(base, demuxerHandle + dx::kEnabledEs));
    for (s32 i = 0; i < n; i++) {
        const u32 es_handle = ps_read32(base, demuxerHandle + dx::kSize + 4 * static_cast<u32>(i));
        if (const s32 r = disable_es(ctx, es_handle); r != lv2::CELL_OK) {
            mutex_unlock(ctx, mhd);
            restore();
            return finish(r, call);
        }
        ps_write32(base, es_handle + es::kDmuxHandle, 0);
        ps_write32(base, demuxerHandle + dx::kEnabledEs, ps_read32(base, demuxerHandle + dx::kEnabledEs) - 1);
    }
    s32 r = mutex_unlock(ctx, mhd);
    if (r == lv2::CELL_OK) r = get_error(dmux::core_close(ctx, ps_read32(base, demuxerHandle + dx::kCoreHandle)));
    if (r == lv2::CELL_OK) r = lv2::sys_mutex_destroy(thread_of(ctx), mhd);
    if (r != lv2::CELL_OK) restore();
    return finish(r, call);
}

s32 cellDmuxSetStream(PPUContext& ctx, u32 demuxerHandle, u32 streamAddress, u32 streamSize, bool discontinuity,
    u64 userData)
{
    const std::string call = std::format("cellDmuxSetStream({:#x}, {:#x}, {:#x}, {}, {:#x})", demuxerHandle,
        streamAddress, streamSize, discontinuity, userData);
    if (!live_handle(ctx, demuxerHandle, call) || streamSize == 0 || streamSize == 0xFFFFFFFF)
        return finish(error(CELL_DMUX_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    if (!(ps_read32(base, demuxerHandle + dx::kState) & DMUX_STOPPED)) return finish(error(CELL_DMUX_ERROR_BUSY), call);
    const u32 mhd = ps_read32(base, demuxerHandle + dx::kMutex);
    if (const s32 r = mutex_lock(ctx, mhd); r != lv2::CELL_OK) return finish(r, call);
    if (const s32 r = get_error(dmux::core_set_stream(ctx, ps_read32(base, demuxerHandle + dx::kCoreHandle),
            streamAddress, streamSize, discontinuity, userData));
        r != lv2::CELL_OK)
        return finish(unlock_with(ctx, mhd, r), call);
    ps_write8(base, demuxerHandle + dx::kStreamIsSet, 1);
    ps_write32(base, demuxerHandle + dx::kState, DMUX_RUNNING);
    ps_write64(base, demuxerHandle + dx::kUserData, userData);
    return finish(mutex_unlock(ctx, mhd), call);
}

s32 cellDmuxResetStream(PPUContext& ctx, u32 demuxerHandle)
{
    const std::string call = std::format("cellDmuxResetStream({:#x})", demuxerHandle);
    if (!live_handle(ctx, demuxerHandle, call)) return finish(error(CELL_DMUX_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    const u32 mhd = ps_read32(base, demuxerHandle + dx::kMutex);
    if (const s32 r = mutex_lock(ctx, mhd); r != lv2::CELL_OK) return finish(r, call);
    const u32 state = ps_read32(base, demuxerHandle + dx::kState);
    if (const s32 r = mutex_unlock(ctx, mhd); r != lv2::CELL_OK) return finish(r, call);
    if (!(state & DMUX_RUNNING) || !ps_read8(base, demuxerHandle + dx::kStreamIsSet))
        return finish(error(CELL_DMUX_ERROR_SEQ), call);
    if (const s32 r = get_error(dmux::core_reset_stream(ctx, ps_read32(base, demuxerHandle + dx::kCoreHandle)));
        r != lv2::CELL_OK)
        return finish(r, call);
    ps_write8(base, demuxerHandle + dx::kStreamIsSet, 0);
    return finish(lv2::CELL_OK, call);
}

s32 cellDmuxResetStreamAndWaitDone(PPUContext& ctx, u32 demuxerHandle)
{
    const std::string call = std::format("cellDmuxResetStreamAndWaitDone({:#x})", demuxerHandle);
    if (!live_handle(ctx, demuxerHandle, call)) return finish(error(CELL_DMUX_ERROR_ARG), call);
    // LLE leaves stream_is_set as it is (RPCS3).
    return finish(get_error(dmux::core_reset_stream_and_wait_done(ctx,
                      ps_read32(guest_base(ctx), demuxerHandle + dx::kCoreHandle))),
        call);
}

s32 cellDmuxQueryEsAttr(PPUContext& ctx, u32 demuxerType, u32 esFilterId, u32 esSpecificInfo, u32 esAttr)
{
    const std::string call = std::format("cellDmuxQueryEsAttr({:#x}, {:#x}, {:#x}, {:#x})", demuxerType, esFilterId,
        esSpecificInfo, esAttr);
    u8* base = guest_base(ctx);
    if (!demuxerType || !usable(ctx, demuxerType, 12, call) ||
        static_cast<s32>(ps_read32(base, demuxerType)) != CELL_DMUX_STREAM_TYPE_PAMF || !esFilterId || !esAttr ||
        !usable(ctx, esAttr, 4, call))
        return finish(error(CELL_DMUX_ERROR_ARG), call);
    dmux::PamfEsAttr core;
    if (const s32 r = get_error(dmux::core_query_es_attr(ctx, esFilterId, esSpecificInfo, core)); r != lv2::CELL_OK)
        return finish(r, call);
    ps_write32(base, esAttr,
        align16(es::kSize + (core.au_queue_max_size + 1) * (core.specific_info_size + es::kElem)) + core.mem_size + 0xf);
    return finish(lv2::CELL_OK, call);
}

s32 cellDmuxQueryEsAttr2(PPUContext& ctx, u32 demuxerType2, u32 esFilterId, u32 esSpecificInfo, u32 esAttr)
{
    const std::string call = std::format("cellDmuxQueryEsAttr2({:#x}, ...)", demuxerType2);
    if (!demuxerType2 || !usable(ctx, demuxerType2, 8, call)) return finish(error(CELL_DMUX_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    StackVar type(ctx, base, 12); // CellDmuxType {streamType}
    ps_write32(base, type.addr(), ps_read32(base, demuxerType2));
    return cellDmuxQueryEsAttr(ctx, type.addr(), esFilterId, esSpecificInfo, esAttr);
}

s32 cellDmuxEnableEs(PPUContext& ctx, u32 demuxerHandle, u32 esFilterId, u32 esResourceInfo, u32 esCb,
    u32 esSpecificInfo, u32 esHandle)
{
    const std::string call = std::format("cellDmuxEnableEs({:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x})", demuxerHandle,
        esFilterId, esResourceInfo, esCb, esSpecificInfo, esHandle);
    u8* base = guest_base(ctx);
    if (!live_handle(ctx, demuxerHandle, call) || !esFilterId || !usable(ctx, esFilterId, 16, call) ||
        !esResourceInfo || !usable(ctx, esResourceInfo, 8, call) || !ps_read32(base, esResourceInfo) ||
        ps_read32(base, esResourceInfo + 4) == 0xFFFFFFFF || !esCb || !usable(ctx, esCb, 8, call) ||
        !ps_read32(base, esCb) || !esHandle || !usable(ctx, esHandle, 4, call))
        return finish(error(CELL_DMUX_ERROR_ARG), call);
    const u32 mhd = ps_read32(base, demuxerHandle + dx::kMutex);
    if (const s32 r = mutex_lock(ctx, mhd); r != lv2::CELL_OK) return finish(r, call);
    if (static_cast<s32>(ps_read32(base, demuxerHandle + dx::kEnabledEs)) >=
        static_cast<s32>(ps_read32(base, demuxerHandle + dx::kMaxEs)))
        return finish(unlock_with(ctx, mhd, error(CELL_DMUX_ERROR_ARG)), call);
    {
        StackVar attr(ctx, base, 4);
        if (const s32 r = cellDmuxQueryEsAttr(ctx, demuxerHandle + dx::kType, esFilterId, esSpecificInfo, attr.addr());
            r != lv2::CELL_OK)
            return finish(unlock_with(ctx, mhd, r), call);
        if (ps_read32(base, attr.addr()) > ps_read32(base, esResourceInfo + 4))
            return finish(unlock_with(ctx, mhd, error(CELL_DMUX_ERROR_ARG)), call);
    }
    dmux::PamfEsAttr core;
    if (const s32 r = get_error(dmux::core_query_es_attr(ctx, esFilterId, esSpecificInfo, core)); r != lv2::CELL_OK)
        return finish(unlock_with(ctx, mhd, r), call);
    core.au_queue_max_size++; // a slot for the access unit a flush makes, so that flushing always succeeds
    const u32 mem_addr = ps_read32(base, esResourceInfo), mem_size = ps_read32(base, esResourceInfo + 4);
    if (!usable(ctx, mem_addr, mem_size, "the stream's memory")) return finish(unlock_with(ctx, mhd, error(CELL_DMUX_ERROR_ARG)), call);
    const u32 es_handle = align16(mem_addr);
    const u32 elements_size = core.au_queue_max_size * (core.specific_info_size + es::kElem);
    const u32 core_mem = align16(es_handle + es::kSize + elements_size);
    if (const s32 r = create_mutex(ctx, es_handle + es::kMutex, "_dx_mes"); r != lv2::CELL_OK)
        return finish(unlock_with(ctx, mhd, r), call);
    const u32 mes = ps_read32(base, es_handle + es::kMutex);
    if (const s32 r = mutex_lock(ctx, mes); r != lv2::CELL_OK) {
        lv2::sys_mutex_destroy(thread_of(ctx), mes);
        return finish(unlock_with(ctx, mhd, r), call);
    }
    u32 core_es_handle = 0;
    if (const s32 r = get_error(dmux::core_enable_es(ctx, ps_read32(base, demuxerHandle + dx::kCoreHandle), esFilterId,
            core_mem, mem_size - (core_mem - mem_addr), es_handle, esSpecificInfo, core_es_handle));
        r != lv2::CELL_OK) {
        const s32 unlocked = mutex_unlock(ctx, mes), destroyed = lv2::sys_mutex_destroy(thread_of(ctx), mes);
        if (unlocked != lv2::CELL_OK) return finish(unlock_with(ctx, mhd, unlocked), call);
        if (destroyed != lv2::CELL_OK) return finish(unlock_with(ctx, mhd, destroyed), call);
        return finish(unlock_with(ctx, mhd, r), call);
    }
    const u32 index = ps_read32(base, demuxerHandle + dx::kEnabledEs);
    ps_write32(base, es_handle + es::kIsEnabled, 1);
    ps_write32(base, es_handle + es::kErrorMemSize, 0);
    ps_write32(base, es_handle + es::kErrorCount, 0);
    ps_write32(base, es_handle + es::kThis, es_handle);
    ps_write32(base, es_handle + es::kThisSize, es::kSize + elements_size);
    ps_write32(base, es_handle + es::kThisIndex, index);
    ps_write32(base, es_handle + es::kDmuxHandle, demuxerHandle);
    ps_write32(base, es_handle + es::kCbFunc, ps_read32(base, esCb));
    ps_write32(base, es_handle + es::kCbArg, ps_read32(base, esCb + 4));
    ps_write32(base, es_handle + es::kCoreEsHandle, core_es_handle);
    ps_write32(base, es_handle + es::kFlushStarted, 0);
    ps_write32(base, es_handle + es::kQMax, core.au_queue_max_size);
    for (u32 off : {es::kQAllocated, es::kQSize, es::kQFront, es::kQBack, es::kQAllocatedBack}) ps_write32(base, es_handle + off, 0);
    const u32 elems = elements(es_handle);
    for (u32 i = 0; i < core.au_queue_max_size; i++) {
        const u32 e = elems + i * es::kElem;
        ps_write32(base, e + es::kElIndex, i);
        ps_write8(base, e + es::kElUnk, 0);
        ps_write32(base, e + es::kElAuAddr, 0);
        ps_write32(base, e + es::kElAuMaxSize, 0);
        ps_write32(base, e + es::kElSpecific, elems + core.au_queue_max_size * es::kElem + i * core.specific_info_size);
        ps_write32(base, e + es::kElSpecificSize, core.specific_info_size);
    }
    ps_write32(base, demuxerHandle + dx::kEnabledEs, index + 1);
    ps_write32(base, demuxerHandle + dx::kSize + 4 * index, es_handle);
    ps_write32(base, esHandle, es_handle);
    if (const s32 r = mutex_unlock(ctx, mes); r != lv2::CELL_OK) {
        lv2::sys_mutex_destroy(thread_of(ctx), mes);
        return finish(unlock_with(ctx, mhd, r), call);
    }
    return finish(mutex_unlock(ctx, mhd), call);
}

s32 cellDmuxDisableEs(PPUContext& ctx, u32 esHandle)
{
    const std::string call = std::format("cellDmuxDisableEs({:#x})", esHandle);
    if (!live_es(ctx, esHandle, call)) return finish(error(CELL_DMUX_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    const u32 dmux = ps_read32(base, esHandle + es::kDmuxHandle);
    const u32 mhd = ps_read32(base, dmux + dx::kMutex);
    if (const s32 r = mutex_lock(ctx, mhd); r != lv2::CELL_OK) return finish(r, call);
    if (const s32 r = disable_es(ctx, esHandle); r != lv2::CELL_OK) return finish(unlock_with(ctx, mhd, r), call);
    // The handles after it move up one.
    const u32 n = ps_read32(base, dmux + dx::kEnabledEs), handles = dmux + dx::kSize;
    u32 at = 0;
    while (at < n && ps_read32(base, handles + 4 * at) != esHandle) at++;
    for (u32 i = at; i + 1 < n; i++) ps_write32(base, handles + 4 * i, ps_read32(base, handles + 4 * (i + 1)));
    ps_write32(base, dmux + dx::kEnabledEs, n - 1);
    if (n) ps_write32(base, handles + 4 * (n - 1), 0);
    return finish(mutex_unlock(ctx, mhd), call);
}

s32 cellDmuxResetEs(PPUContext& ctx, u32 esHandle)
{
    const std::string call = std::format("cellDmuxResetEs({:#x})", esHandle);
    if (!live_es(ctx, esHandle, call)) return finish(error(CELL_DMUX_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    const u32 dmux = ps_read32(base, esHandle + es::kDmuxHandle);
    const u32 mhd = ps_read32(base, dmux + dx::kMutex), mes = ps_read32(base, esHandle + es::kMutex);
    if (const s32 r = mutex_lock(ctx, mhd); r != lv2::CELL_OK) return finish(r, call);
    const u32 state = ps_read32(base, dmux + dx::kState);
    if (const s32 r = mutex_unlock(ctx, mhd); r != lv2::CELL_OK) return finish(r, call);
    if (!(state & DMUX_STOPPED)) return finish(error(CELL_DMUX_ERROR_SEQ), call);
    if (const s32 r = mutex_lock(ctx, mes); r != lv2::CELL_OK) return finish(r, call);
    if (const s32 r = get_error(dmux::core_reset_es(ctx, ps_read32(base, esHandle + es::kCoreEsHandle))); r != lv2::CELL_OK)
        return finish(unlock_with(ctx, mes, r), call);
    const u32 max = ps_read32(base, esHandle + es::kQMax);
    for (u32 i = 0; i < max; i++) {
        const u32 e = elements(esHandle) + i * es::kElem;
        ps_write32(base, e + es::kElIndex, i);
        ps_write8(base, e + es::kElUnk, 0);
        ps_write32(base, e + es::kElAuAddr, 0);
        ps_write32(base, e + es::kElAuMaxSize, 0);
    }
    for (u32 off : {es::kErrorMemSize, es::kErrorCount, es::kQAllocated, es::kQSize, es::kQFront, es::kQBack,
             es::kQAllocatedBack})
        ps_write32(base, esHandle + off, 0);
    return finish(mutex_unlock(ctx, mes), call);
}

s32 cellDmuxGetAu(PPUContext& ctx, u32 esHandle, u32 auInfo, u32 auSpecificInfo)
{
    const std::string call = std::format("cellDmuxGetAu({:#x}, {:#x}, {:#x})", esHandle, auInfo, auSpecificInfo);
    const s32 r = pop_au(ctx, esHandle, auInfo, auSpecificInfo, false, call);
    if (r != error(CELL_DMUX_ERROR_EMPTY)) finish(r, call); // empty is the normal answer between units
    return r;
}

s32 cellDmuxPeekAu(PPUContext& ctx, u32 esHandle, u32 auInfo, u32 auSpecificInfo)
{
    const std::string call = std::format("cellDmuxPeekAu({:#x}, {:#x}, {:#x})", esHandle, auInfo, auSpecificInfo);
    const s32 r = pop_au(ctx, esHandle, auInfo, auSpecificInfo, true, call);
    if (r != error(CELL_DMUX_ERROR_EMPTY)) finish(r, call);
    return r;
}

s32 cellDmuxGetAuEx(PPUContext& ctx, u32 esHandle, u32 auInfoEx, u32 auSpecificInfo)
{
    return cellDmuxGetAu(ctx, esHandle, auInfoEx, auSpecificInfo);
}

s32 cellDmuxPeekAuEx(PPUContext& ctx, u32 esHandle, u32 auInfoEx, u32 auSpecificInfo)
{
    return cellDmuxPeekAu(ctx, esHandle, auInfoEx, auSpecificInfo);
}

s32 cellDmuxReleaseAu(PPUContext& ctx, u32 esHandle)
{
    const std::string call = std::format("cellDmuxReleaseAu({:#x})", esHandle);
    if (!live_es(ctx, esHandle, call)) return finish(error(CELL_DMUX_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    const u32 mes = ps_read32(base, esHandle + es::kMutex);
    if (const s32 r = mutex_lock(ctx, mes); r != lv2::CELL_OK) return finish(r, call);
    const auto rd = [&](u32 off) { return ps_read32(base, esHandle + off); };
    const auto wr = [&](u32 off, u32 v) { ps_write32(base, esHandle + off, v); };
    u32 mem_addr = 0, mem_size = 0;
    if (static_cast<s32>(rd(es::kQAllocated)) < 1) {
        if (rd(es::kErrorCount) == 0) return finish(unlock_with(ctx, mes, error(CELL_DMUX_ERROR_SEQ)), call);
        mem_addr = rd(es::kErrorMemAddr); // retrying a release the core refused
        mem_size = rd(es::kErrorMemSize);
    } else {
        const u32 e = elements(esHandle) + rd(es::kQAllocatedBack) * es::kElem;
        mem_size = rd(es::kErrorMemSize) + ps_read32(base, e + es::kElAuSize);
        wr(es::kErrorMemSize, mem_size);
        mem_addr = rd(es::kErrorCount) == 0 ? ps_read32(base, e + es::kElAuAddr) : rd(es::kErrorMemAddr);
        const u32 max = rd(es::kQMax);
        wr(es::kQAllocatedBack, (rd(es::kQAllocatedBack) + 1) % max);
        wr(es::kQAllocated, rd(es::kQAllocated) - 1);
        if (static_cast<s32>(rd(es::kQAllocated)) < static_cast<s32>(rd(es::kQSize))) {
            wr(es::kQFront, (rd(es::kQFront) + 1) % max);
            wr(es::kQSize, rd(es::kQSize) - 1);
        }
    }
    if (const s32 r = get_error(dmux::core_release_au(ctx, rd(es::kCoreEsHandle), mem_addr, mem_size)); r != lv2::CELL_OK) {
        if (rd(es::kErrorCount) == 0) wr(es::kErrorMemAddr, mem_addr);
        wr(es::kErrorCount, rd(es::kErrorCount) + 1);
        return finish(unlock_with(ctx, mes, r), call);
    }
    wr(es::kErrorCount, 0);
    wr(es::kErrorMemSize, 0);
    const s32 r = mutex_unlock(ctx, mes);
    if (r != lv2::CELL_OK) finish(r, call);
    return r;
}

s32 cellDmuxFlushEs(PPUContext& ctx, u32 esHandle)
{
    const std::string call = std::format("cellDmuxFlushEs({:#x})", esHandle);
    if (!live_es(ctx, esHandle, call)) return finish(error(CELL_DMUX_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    const u32 dmux = ps_read32(base, esHandle + es::kDmuxHandle);
    const u32 mhd = ps_read32(base, dmux + dx::kMutex);
    if (const s32 r = mutex_lock(ctx, mhd); r != lv2::CELL_OK) return finish(r, call);
    const u32 state = ps_read32(base, dmux + dx::kState);
    if (const s32 r = mutex_unlock(ctx, mhd); r != lv2::CELL_OK) return finish(r, call);
    if (!(state & DMUX_STOPPED)) return finish(error(CELL_DMUX_ERROR_SEQ), call);
    ps_write32(base, esHandle + es::kFlushStarted, ps_read32(base, esHandle + es::kFlushStarted) | 1);
    if (const s32 r = get_error(dmux::core_flush_es(ctx, ps_read32(base, esHandle + es::kCoreEsHandle))); r != lv2::CELL_OK) {
        ps_write32(base, esHandle + es::kFlushStarted, ps_read32(base, esHandle + es::kFlushStarted) & ~1u);
        return finish(r, call);
    }
    return finish(lv2::CELL_OK, call);
}

using dmux::_pspocket_dmux_pamf_thread; // registered under its own name, which its NID is computed from

void register_cellDmux(Registry& r)
{
    r.add("cellDmux", {
        PS_HLE_FUNC(0xA2D4189B, cellDmuxQueryAttr),
        PS_HLE_FUNC(0x3F76E3CD, cellDmuxQueryAttr2),
        PS_HLE_FUNC(0x68492DE9, cellDmuxOpen),
        PS_HLE_FUNC(0xF6C23560, cellDmuxOpenEx),
        PS_HLE_FUNC(0xE075FABC, cellDmuxOpenExt),
        PS_HLE_FUNC(0x11BC3A6C, cellDmuxOpen2),
        PS_HLE_FUNC(0x8C692521, cellDmuxClose),
        PS_HLE_FUNC(0x04E7499F, cellDmuxSetStream),
        PS_HLE_FUNC(0x5D345DE9, cellDmuxResetStream),
        PS_HLE_FUNC(0xCCFF1284, cellDmuxResetStreamAndWaitDone),
        PS_HLE_FUNC(0x02170D1A, cellDmuxQueryEsAttr),
        PS_HLE_FUNC(0x52911BCF, cellDmuxQueryEsAttr2),
        PS_HLE_FUNC(0x7B56DC3F, cellDmuxEnableEs),
        PS_HLE_FUNC(0x05371C8D, cellDmuxDisableEs),
        PS_HLE_FUNC(0x21D424F0, cellDmuxResetEs),
        PS_HLE_FUNC(0x42C716B5, cellDmuxGetAu),
        PS_HLE_FUNC(0x2750C5E0, cellDmuxPeekAu),
        PS_HLE_FUNC(0x2C9A5857, cellDmuxGetAuEx),
        PS_HLE_FUNC(0x002E8DA2, cellDmuxPeekAuEx),
        PS_HLE_FUNC(0x24EA6474, cellDmuxReleaseAu),
        PS_HLE_FUNC(0xEBB3B2BD, cellDmuxFlushEs),
        PS_HLE_INTERNAL(dmux::NID_dmux_pamf_thread, _pspocket_dmux_pamf_thread),
    });
    dmux::register_pamf_table(r); // the core's own table, for other modules (D-592)
}

} // namespace pspocket::hle
