// cellSail's dmux pipeline (D-562 part 13, D-919), as libsail (fw 4.92) has it: the player's demultiplexer object
// (0x58 bytes at the player's +0x300) over pspocket's cellDmux, and each elementary stream's part of it (0x70 bytes at
// the stream slot's +0x10). cellDmux's callbacks become messages to the player's control thread: an item demultiplexed
// (3) or a fatal error (0xD2) from the stream, an AU found (4) or an ES flushed (0x2F) from an elementary stream.
//
// MP4 and AVI streams go through the firmware's MP4 and AVI libraries' tables instead; that part is not pspocket's
// (a stream of those types stops the process with what was asked).

#include <cstring>
#include <format>

#include "cellSail.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {

namespace {

using namespace sail;

constexpr u32 kDmuxBusy = 0x80610203; // CELL_DMUX_ERROR_BUSY: libsail stops retrying at once

u32 internal(std::string_view name) { return HleProcess::current()->link_function("cellSail", recomp::nid_of(name), name); }

bool container(PPUContext& ctx, u32 dm)
{
    const u32 kind = r32(ctx, dm + dmx::kType);
    return kind == 0x81 || kind == 0x82;
}

void container_part(PPUContext& ctx, u32 dm, std::string_view what)
{
    later_part(ctx, std::format("{} of an {} stream (the firmware library's table)", what,
        r32(ctx, dm + dmx::kType) == 0x81 ? "MP4" : "AVI"));
}

u32 player_of(PPUContext& ctx, u32 dm) { return r32(ctx, dm + dmx::kPlayer); }

// cellDmuxDisableEs, again up to ten times 500 us apart unless it answers busy.
s32 disable_es_retrying(PPUContext& ctx, u32 handle)
{
    s32 r = 0;
    for (u32 tries = 10; tries; tries--) {
        r = static_cast<s32>(module_call(ctx, "cellDmux", "cellDmuxDisableEs", {handle}));
        if (r >= 0) return r;
        if (static_cast<u32>(r) == kDmuxBusy) return r;
        sc(ctx, 141, {500});
    }
    return r;
}

// The stream's elementary stream disabled (its handle cleared, the pipeline's enabled count down); 1 when it was not.
s32 es_disable(PPUContext& ctx, u32 es)
{
    const u32 dm = r32(ctx, es + des::kPipeline);
    if (container(ctx, dm)) container_part(ctx, dm, "disabling an elementary stream");
    const u32 h = r32(ctx, es + des::kHandle);
    if (!h) return 1;
    const s32 r = disable_es_retrying(ctx, h);
    if (r < 0) return r;
    w32(ctx, es + des::kHandle, 0);
    w32(ctx, dm + dmx::kEnabled, r32(ctx, dm + dmx::kEnabled) - 1);
    return lv2::CELL_OK;
}

// cellDmuxEnableEs with the stream's memory and libsail's ES callback, again up to ten times 500 us apart unless busy;
// 1 when enabled already, INVALID_STATE without its memory.
s32 es_enable(PPUContext& ctx, u32 es)
{
    const u32 dm = r32(ctx, es + des::kPipeline);
    if (container(ctx, dm)) container_part(ctx, dm, "enabling an elementary stream");
    if (!r32(ctx, es + des::kMemory)) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (r32(ctx, es + des::kHandle)) return 1;
    StackVar v(ctx, gb(ctx), 0x20); // +0 the handle, +4 the callback {function, argument}, +0xC the resource
    const u32 handle = v.addr(), cb = v.addr() + 4, res = v.addr() + 0xC;
    w32(ctx, res, r32(ctx, es + des::kMemory));
    w32(ctx, res + 4, r32(ctx, es + des::kMemorySize));
    w32(ctx, cb, internal("_pspocket_cellSail_dmuxEsNotify"));
    w32(ctx, cb + 4, es);
    s32 r = 0;
    for (u32 tries = 10; tries; tries--) {
        w32(ctx, es + des::kFound, 0);
        w32(ctx, es + des::kHeld, 0);
        w8(ctx, es + des::kHasSaved, 0);
        r = static_cast<s32>(module_call(ctx, "cellDmux", "cellDmuxEnableEs", {r32(ctx, dm + dmx::kHandle),
            es + des::kFilter, res, cb, es + des::kSpecific, handle}));
        if (r >= 0) {
            w32(ctx, es + des::kHandle, r32(ctx, handle));
            w32(ctx, dm + dmx::kEnabled, r32(ctx, dm + dmx::kEnabled) + 1);
            return r;
        }
        if (static_cast<u32>(r) == kDmuxBusy) return r;
        sc(ctx, 141, {500});
    }
    return r;
}

} // namespace

namespace sail {

// ------------------------------------------------------------------------------------------------- callbacks

// cellDmux's messages to the pipeline: an item demultiplexed (0) becomes message 3 with the item's record (the high
// word of its user data), a fatal error (1) message 0xD2 {error, 3, 0}.
u32 _pspocket_cellSail_dmuxNotify(PPUContext& ctx, u32, u32 msg, u32 dm)
{
    const u32 type = r32(ctx, msg);
    if (type == 0) post(ctx, player_of(ctx, dm), 3, dm, static_cast<u32>(r64(ctx, msg + 8) >> 32));
    else if (type == 1) post_error(ctx, player_of(ctx, dm), static_cast<s32>(r64(ctx, msg + 8)), 3, 0);
    return 0;
}

// cellDmux's messages to an elementary stream: an AU found (0) becomes message 4, the stream flushed (1) 0x2F.
u32 _pspocket_cellSail_dmuxEsNotify(PPUContext& ctx, u32, u32, u32 msg, u32 es)
{
    const u32 type = r32(ctx, msg), dm = r32(ctx, es + des::kPipeline);
    if (type == 0) post(ctx, player_of(ctx, dm), 4, dm, es);
    else if (type == 1) post(ctx, player_of(ctx, dm), 0x2F, dm, es);
    return 0;
}

// ------------------------------------------------------------------------------------------------- the pipeline

void dmux_init(PPUContext& ctx, u32 dm, u32 p)
{
    std::memset(gb(ctx) + dm, 0, dmx::kSize);
    w32(ctx, dm + dmx::kState, 0);
    w32(ctx, dm + dmx::kPlayer, p);
}

// Open: an open pipeline answers 1; PAMF loads cellDmuxPamf (module 0x2A), asks cellDmux for its memory, takes it from
// the player's allocator and opens the demuxer on the player's SPURS with the player's dmux resource (+0x358: memory,
// size, thread priority and stack, SPURS, SPU priorities, contention); the answer completes `handle`.
void dmux_open(PPUContext& ctx, u32 dm, u32 type, u32 source, u64 handle)
{
    const u32 p = player_of(ctx, dm);
    s32 r = 1;
    if (r32(ctx, dm + dmx::kState) != 1) {
        const u32 kind = r32(ctx, type);
        if (kind == 0x81 || kind == 0x82) later_part(ctx, "MP4 and AVI demuxing (the firmware libraries')");
        r = error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
        if (kind == 1 && (r = load_module(ctx, 0x2A)) >= 0) {
            w32(ctx, dm + dmx::kModule, 0x2A);
            StackVar attr(ctx, gb(ctx), 16);
            r = static_cast<s32>(module_call(ctx, "cellDmux", "cellDmuxQueryAttr", {type, attr.addr()}));
            if (r >= 0) {
                std::memmove(gb(ctx) + dm + dmx::kType, gb(ctx) + type, 12);
                const u32 size = r32(ctx, attr.addr());
                const u32 mem = player_alloc(ctx, p, 0x80, size);
                r = error(CELL_SAIL_ERROR_MEMORY);
                if (mem) {
                    const u32 res = p + pl::kParams;
                    StackVar cb(ctx, gb(ctx), 16);
                    w32(ctx, cb.addr(), internal("_pspocket_cellSail_dmuxNotify"));
                    w32(ctx, cb.addr() + 4, dm);
                    w32(ctx, res + 4, size);
                    w32(ctx, res + 16, r32(ctx, p + pl::kSpurs));
                    w32(ctx, res, mem);
                    r = static_cast<s32>(module_call(ctx, "cellDmux", "cellDmuxOpenExt",
                        {type, res, cb.addr(), dm + dmx::kHandle}));
                    if (r >= 0) {
                        w32(ctx, dm + dmx::kMemory, mem);
                        w32(ctx, dm + dmx::kState, kind);
                        w32(ctx, dm + dmx::kSource, source);
                        post_completion(ctx, p, 0, handle);
                        return;
                    }
                    player_free(ctx, p, 0x80, mem);
                }
            }
            if (const u32 m = r32(ctx, dm + dmx::kModule)) {
                unload_module(ctx, m & 0xFFFF);
                w32(ctx, dm + dmx::kModule, 0);
            }
        }
    }
    post_completion(ctx, p, r, handle);
}

// Close: every elementary stream disabled (a failure an error message) and its memory freed, cellDmuxClose (again up to
// ten times 500 us apart while busy), the pipeline's memory and module released; the answer completes `handle` (none
// at the player's end).
void dmux_close(PPUContext& ctx, u32 dm, u64 handle)
{
    const u32 p = player_of(ctx, dm);
    if (container(ctx, dm)) container_part(ctx, dm, "closing the stream");
    for (u32 es = r32(ctx, dm + dmx::kEsList); es;) {
        if (const u32 h = r32(ctx, es + des::kHandle)) {
            if (const s32 r = disable_es_retrying(ctx, h); r < 0) post_error(ctx, p, r, 3, 0);
        }
        if (const u32 mem = r32(ctx, es + des::kMemory)) {
            player_free(ctx, p, 0x80, mem);
            w32(ctx, es + des::kMemory, 0);
        }
        const u32 next = r32(ctx, es + des::kNext);
        std::memset(gb(ctx) + es, 0, des::kSize);
        es = next;
    }
    w32(ctx, dm + dmx::kEsList, 0);
    s32 result = 0;
    bool closed = true;
    if (r32(ctx, dm + dmx::kHandle)) {
        for (u32 tries = 10;;) {
            const s32 r = static_cast<s32>(module_call(ctx, "cellDmux", "cellDmuxClose", {r32(ctx, dm + dmx::kHandle)}));
            if (r >= 0) break;
            if (static_cast<u32>(r) != kDmuxBusy) {
                post_error(ctx, p, r, 3, 0);
                w32(ctx, dm + dmx::kState, 0xC);
                result = r;
                closed = false;
                break;
            }
            sc(ctx, 141, {500});
            if (--tries == 0) break;
        }
    }
    if (closed) {
        w32(ctx, dm + dmx::kHandle, 0);
        w32(ctx, dm + dmx::kEsList, 0);
        w32(ctx, dm + dmx::kEsCount, 0);
        w32(ctx, dm + dmx::kEnabled, 0);
        w32(ctx, dm + dmx::kState, 0);
    }
    if (const u32 mem = r32(ctx, dm + dmx::kMemory)) {
        player_free(ctx, p, 0x80, mem);
        w32(ctx, dm + dmx::kMemory, 0);
    }
    if (const u32 m = r32(ctx, dm + dmx::kModule)) {
        if (const s32 r = unload_module(ctx, m & 0xFFFF); r < 0) {
            post_error(ctx, p, r, 3, 0);
            result = r;
        }
        w32(ctx, dm + dmx::kModule, 0);
    }
    post_completion(ctx, p, result, handle);
}

void dmux_fini(PPUContext& ctx, u32 dm)
{
    if (r32(ctx, dm + dmx::kType) == 1) dmux_close(ctx, dm, 0);
    w32(ctx, dm + dmx::kState, 0xC);
}

// ------------------------------------------------------------------------------------------------- elementary streams

// Added: its memory (cellDmuxQueryEsAttr's size, from the player's allocator), the part laid out and put first on the
// pipeline's list; enabled when asked. 1 when it was there already; INVALID_STATE before the pipeline is open.
s32 dmux_es_add(PPUContext& ctx, u32 dm, u32 kind, u32 filter, u32 specific, u32 slot, u32 es, bool enable)
{
    if (container(ctx, dm)) container_part(ctx, dm, "adding an elementary stream");
    if (!r32(ctx, dm + dmx::kHandle)) return error(CELL_SAIL_ERROR_INVALID_STATE);
    s32 r = 1;
    if (!r32(ctx, es + des::kMemory)) {
        StackVar attr(ctx, gb(ctx), 16);
        r = static_cast<s32>(module_call(ctx, "cellDmux", "cellDmuxQueryEsAttr",
            {dm + dmx::kType, filter, specific, attr.addr()}));
        if (r < 0) return r;
        const u32 size = r32(ctx, attr.addr());
        const u32 mem = player_alloc(ctx, player_of(ctx, dm), 0x80, size);
        if (!mem) return error(CELL_SAIL_ERROR_MEMORY);
        std::memset(gb(ctx) + es, 0, des::kSize);
        w32(ctx, es + des::kMemorySize, size);
        w32(ctx, es + des::kNext, r32(ctx, dm + dmx::kEsList));
        w32(ctx, es + des::kKind, kind);
        w32(ctx, es + des::kSlot, slot);
        w32(ctx, es + des::kMemory, mem);
        w32(ctx, es + des::kPipeline, dm);
        std::memmove(gb(ctx) + es + des::kFilter, gb(ctx) + filter, 0x10);
        std::memmove(gb(ctx) + es + des::kSpecific, gb(ctx) + specific, 0xC);
        r = 0;
        w32(ctx, dm + dmx::kEsList, es);
        w32(ctx, dm + dmx::kEsCount, r32(ctx, dm + dmx::kEsCount) + 1);
    }
    if (enable) r = es_enable(ctx, es);
    return r;
}

// Removed: disabled first (its answer not looked at), off the pipeline's list, its memory freed. 1 when it was not
// there (no memory); 0 when it has memory but is not on the list.
s32 dmux_es_remove(PPUContext& ctx, u32 dm, u32 es)
{
    if (container(ctx, dm)) container_part(ctx, dm, "removing an elementary stream");
    if (!r32(ctx, es + des::kMemory)) return 1;
    if (r32(ctx, es + des::kHandle)) es_disable(ctx, es);
    for (u32 link = dm + dmx::kEsList, cur; (cur = r32(ctx, link)) != 0; link = cur + des::kNext) {
        if (cur != es) continue;
        const u32 mem = r32(ctx, es + des::kMemory);
        w32(ctx, link, r32(ctx, es + des::kNext));
        w32(ctx, dm + dmx::kEsCount, r32(ctx, dm + dmx::kEsCount) - 1);
        player_free(ctx, player_of(ctx, dm), 0x80, mem);
        w32(ctx, es + des::kNext, 0);
        w32(ctx, es + des::kMemory, 0);
        return lv2::CELL_OK;
    }
    return lv2::CELL_OK;
}

s32 dmux_es_readd(PPUContext& ctx, u32 dm, u32 kind, u32 filter, u32 specific, u32 slot, u32 es)
{
    if (container(ctx, dm)) container_part(ctx, dm, "setting an elementary stream again");
    const s32 r = dmux_es_remove(ctx, dm, es);
    if (r < 0) return r;
    return dmux_es_add(ctx, dm, kind, filter, specific, slot, es, false);
}

} // namespace sail

void register_cellSailDmux(Registry& r)
{
    r.add("cellSail", {
        ::pspocket::hle::Function{{}, "_pspocket_cellSail_dmuxNotify", recomp::nid_of("_pspocket_cellSail_dmuxNotify"),
            &detail::thunk<&sail::_pspocket_cellSail_dmuxNotify>, true},
        ::pspocket::hle::Function{{}, "_pspocket_cellSail_dmuxEsNotify",
            recomp::nid_of("_pspocket_cellSail_dmuxEsNotify"), &detail::thunk<&sail::_pspocket_cellSail_dmuxEsNotify>,
            true},
    });
}

} // namespace pspocket::hle
