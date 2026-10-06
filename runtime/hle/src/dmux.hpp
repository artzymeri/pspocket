#pragma once

// cellDmux and its PAMF core (D-194). On a PS3, libdmux (the game's API) calls libdmuxpamf through a table of core
// operations (CellDmuxCoreOps, the variable 0x28b2b7b2), and libdmuxpamf calls back through function pointers. Both are
// pspocket's own here, so the table and the callbacks are plain C++ calls between cellDmux.cpp and cellDmuxPamf.cpp,
// with the arguments RPCS3's ops take. The core's answers are CellDmuxPamfError values; cellDmux maps them
// (RPCS3 get_error).

#include <array>

#include "pspocket/core/types.hpp"
#include "pspocket/ppu/context.h"

namespace pspocket::hle::dmux {

// CellDmuxPamfError: not error codes but small numbers, as libdmuxpamf has them.
enum : u32 {
    PAMF_OK = 0,
    PAMF_ERROR_BUSY = 1,
    PAMF_ERROR_ARG = 2,
    PAMF_ERROR_UNKNOWN_STREAM = 3,
    PAMF_ERROR_NO_MEMORY = 5,
    PAMF_ERROR_FATAL = 6,
};

constexpr u32 DMUX_PAMF_VERSION = 0x280000;
constexpr s32 DMUX_PAMF_MAX_ENABLED_ES_NUM = 0x40;

struct PamfAttr { // CellDmuxPamfAttr
    u32 max_enabled_es_num = 0, version = 0, mem_size = 0;
};
struct PamfEsAttr { // CellDmuxPamfEsAttr
    u32 au_queue_max_size = 0, mem_size = 0, specific_info_size = 0;
};
struct CoreResource { // CellDmuxResource, as the front end hands it on
    u32 mem_addr = 0, mem_size = 0, ppu_thread_priority = 0, ppu_thread_stack_size = 0, spu_thread_priority = 0,
        num_of_spus = 0;
};
struct CoreResourceSpurs { // CellDmuxResourceSpurs
    u32 spurs = 0;
    u64 priority = 0;
    u32 max_contention = 0;
};
// An access unit found (DmuxPamfAuInfo), as the core gives it to the front end.
struct AuInfo {
    u32 addr = 0, size = 0;
    u64 pts = 0, dts = 0, user_data = 0;
    std::array<u8, 0x10> specific_info{};
    u32 specific_info_size = 0;
    bool is_rap = false;
};

// ---- The core (cellDmuxPamf.cpp): RPCS3's _CellDmuxCoreOp* functions. Addresses are the game's memory; a core
// handle is its CellDmuxPamfHandle, an ES handle its CellDmuxPamfEsHandle.
u32 core_query_attr(PPUContext& ctx, u32 pamf_specific_info, PamfAttr& attr);
u32 core_open(PPUContext& ctx, u32 pamf_specific_info, const CoreResource& res, const CoreResourceSpurs* spurs,
    u32 dmux_handle, u32& core_handle);
u32 core_close(PPUContext& ctx, u32 core_handle);
u32 core_reset_stream(PPUContext& ctx, u32 core_handle);
u32 core_set_stream(PPUContext& ctx, u32 core_handle, u32 stream_address, u32 stream_size, bool discontinuity,
    u64 user_data);
u32 core_release_au(PPUContext& ctx, u32 core_es_handle, u32 au_addr, u32 au_size);
u32 core_query_es_attr(PPUContext& ctx, u32 es_filter_id, u32 es_specific_info, PamfEsAttr& attr);
u32 core_enable_es(PPUContext& ctx, u32 core_handle, u32 es_filter_id, u32 mem_addr, u32 mem_size, u32 es_handle,
    u32 es_specific_info, u32& core_es_handle);
u32 core_disable_es(PPUContext& ctx, u32 core_es_handle);
u32 core_flush_es(PPUContext& ctx, u32 core_es_handle);
u32 core_reset_es(PPUContext& ctx, u32 core_es_handle);
u32 core_reset_stream_and_wait_done(PPUContext& ctx, u32 core_handle);

// The core's PPU thread ("_dmux_pamf_ppu_thread"): an internal function of cellDmux, linked with it.
constexpr u32 NID_dmux_pamf_thread = 0x2FF13233; // nid_of("_pspocket_dmux_pamf_thread"): pspocket's own
void _pspocket_dmux_pamf_thread(PPUContext& ctx, u64 core_handle);

// ---- The front end's notifications (cellDmux.cpp), called on the core's PPU thread: RPCS3 notify_*.
void notify_demux_done(PPUContext& ctx, u32 dmux_handle);
void notify_fatal_err(PPUContext& ctx, u32 error, u32 dmux_handle);
void notify_prog_end_code(PPUContext& ctx, u32 dmux_handle);
u32 notify_es_au_found(PPUContext& ctx, const AuInfo& au, u32 es_handle); // 0, or 1: its queue is full
void notify_es_flush_done(PPUContext& ctx, u32 es_handle);

} // namespace pspocket::hle::dmux
