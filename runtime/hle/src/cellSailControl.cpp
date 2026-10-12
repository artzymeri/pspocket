// cellSail's control thread (D-562 part 6, D-919): Boot and the player's thread, as libsail (fw 4.92) has them. Boot
// checks every stream has its renderer (or adapter, or AU receiver), keeps the game's argument and starts
// "_libsail-control" (priority and stack from the player). The thread holds the player's lock while it sets up: SPURS
// of its own when the game gave none (4 KB, "_libsail" its threads' prefix, the player's SPU count and priorities), the
// dmux pipeline, then state 2 and the events (state changed to 2; boot completed with the game's argument); it releases
// the lock and runs the message loop: take the next message (waiting 3 s at a time), dispatch by its code. Stop (0xD1,
// posted by Finalize) ends the loop; the thread takes the lock again, takes the player down (the dmux pipeline, the
// descriptors), releases it, finalizes and frees its SPURS, sets state 10 and exits. A failure in set-up sends state 9
// and boot completed with the error. The descriptors' and streams' messages are here, the elementary streams' in
// cellSailEs.cpp; a message whose handler is not here yet stops the process with its code.

#include <cmath>
#include <cstring>
#include <format>

#include "cellSail.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace sail {

constexpr u32 kStackStat = 0x3D0; // the deepest the event handler's stack went (u64), libsail's
constexpr u32 kMessage = 0x19C;   // the message being handled (200 while waiting)
constexpr u32 kBootRunning = 0x250;
constexpr u32 kCond = pl::kLock + 4; // the condition the game's thread waits on for a command's answer
constexpr u32 kNextDescriptor = 0x228, kDescriptors = 0x3BC;

s32 player_lock(PPUContext& ctx, u32 p) { return sc(ctx, 102, {r32(ctx, p + pl::kLock), 0}); }
s32 player_unlock(PPUContext& ctx, u32 p) { return sc(ctx, 104, {r32(ctx, p + pl::kLock)}); }

u32 player_alloc(PPUContext& ctx, u32 p, u32 boundary, u32 size)
{
    return static_cast<u32>(call_guest(ctx, r32(ctx, p + pl::kAlloc), {r32(ctx, p + pl::kAllocArg), boundary, size}));
}

void player_free(PPUContext& ctx, u32 p, u32 boundary, u32 ptr)
{
    call_guest(ctx, r32(ctx, p + pl::kFree), {r32(ctx, p + pl::kAllocArg), boundary, ptr});
}

// ------------------------------------------------------------------------------------------------- events

// The game's handler(arg, event, a, b), the event (major << 32 | minor); the depth of the control thread's stack at
// the call kept, as libsail does.
static void notify(PPUContext& ctx, u32 p, u64 event, u64 a, u64 b)
{
    const u32 handler = r32(ctx, p + pl::kHandler);
    if (!handler) return;
    const u64 used = r64(ctx, p + pl::kFrame) - ctx.gpr[1];
    if (r64(ctx, p + kStackStat) < used) w64(ctx, p + kStackStat, used);
    call_guest(ctx, handler, {r32(ctx, p + pl::kHandlerArg), event, a, b});
}

// An event of `major` (3: the state changed) when the game subscribed to it.
void notify_event(PPUContext& ctx, u32 p, u32 major, u64 a, u64 b)
{
    if (r64(ctx, p + pl::kEventMask) & (u64{1} << major)) notify(ctx, p, u64{major} << 32, a, b);
}

// A command completed (major 2, minor the command): sent when the command is the one the thread waits on.
void notify_completed(PPUContext& ctx, u32 p, u32 command, s32 result)
{
    if (r32(ctx, p + kBootRunning) != command) return;
    const u64 arg = r64(ctx, p + pl::kBootArg);
    w32(ctx, p + kBootRunning, 0);
    w64(ctx, p + pl::kBootArg, 0);
    if (!(r64(ctx, p + pl::kEventMask) & 4)) return;
    notify(ctx, p, u64{2} << 32 | command, static_cast<u64>(static_cast<s64>(result)), arg);
}

// An error (major 1) when the game subscribed to errors: "error 0x%X" printed first, as libsail does.
void notify_error(PPUContext& ctx, u32 p, s32 err)
{
    if (!(r64(ctx, p + pl::kEventMask) & 2)) return;
    StackVar text(ctx, gb(ctx), 16);
    std::memcpy(gb(ctx) + text.addr(), "error 0x%X\n", 11);
    lv2_lib(ctx, "_sys_printf", {text.addr(), static_cast<u64>(static_cast<s64>(err))});
    notify(ctx, p, u64{1} << 32, static_cast<u64>(static_cast<s64>(err)), 0);
}

// ------------------------------------------------------------------------------------------------- commands

// The game's side of a command (libsail's): under the player's lock, a reply record {the answer's address, the calling
// thread} posted at the queue's head with the command, then one wait on the player's condition, which the control
// thread signals to this thread once it has answered. The lock's error, or 0 with the answer at `reply`.
s32 command(PPUContext& ctx, u32 p, u32 code, u32 arg, u32 reply)
{
    const s32 r = player_lock(ctx, p);
    if (r < 0) return r;
    StackVar record(ctx, gb(ctx), 16);
    w32(ctx, record.addr(), reply);
    lv2_lib(ctx, "sys_ppu_thread_get_id", {record.addr() + 8});
    post_urgent(ctx, p, code, 0, arg, record.addr());
    sc(ctx, 107, {r32(ctx, p + kCond), 0});
    player_unlock(ctx, p);
    return 0;
}

// A command and its answer: the command's error, or the answer.
s32 command_answer(PPUContext& ctx, u32 p, u32 code, u32 arg)
{
    StackVar reply(ctx, gb(ctx), 16);
    const s32 r = command(ctx, p, code, arg, reply.addr());
    return r < 0 ? r : static_cast<s32>(r32(ctx, reply.addr()));
}

// The control thread's side: the answer stored, the waiting thread signalled (sys_cond_signal_to), the lock released.
// A failed signal is an error and a fatal error, the lock kept (libsail's).
bool answer(PPUContext& ctx, u32 p, u32 record, s32 result)
{
    w32(ctx, r32(ctx, record), static_cast<u32>(result));
    const s32 r = sc(ctx, 110, {r32(ctx, p + kCond), r64(ctx, record + 8)});
    if (r < 0) {
        notify_error(ctx, p, r);
        notify_error(ctx, p, error(CELL_SAIL_ERROR_FATAL));
        return false;
    }
    player_unlock(ctx, p);
    return true;
}

// A pipeline's error for the player (libsail's): message 0xD2 {error, kind, index}.
s32 post_error(PPUContext& ctx, u32 p, s32 err, u32 kind, u32 index)
{
    return post_message(ctx, p, 0xD2, static_cast<u32>(err), kind, index);
}

void later_part(PPUContext& ctx, std::string_view what)
{
    log_call_site(ctx, gb(ctx), std::format("cellSail: {}: not pspocket's yet", what));
    PS_FATAL(kChan, "libsail's {} is a later part of D-919", what);
}

// A call's completion event (libsail's), under the player's lock.
void call_completed(PPUContext& ctx, u32 p, u32 call, s32 result)
{
    if (const s32 r = player_lock(ctx, p); r < 0) {
        notify_error(ctx, p, r);
        return;
    }
    notify_completed(ctx, p, call, result);
    player_unlock(ctx, p);
}

// ------------------------------------------------------------------------------------------------- system modules

// Whether libsail loads the modules it needs: its module_start asks lv2 for the process's authority (system software,
// program types 1 and 2, has them loaded already).
bool loads_modules(PPUContext& ctx)
{
    const u32 pid = static_cast<u32>(sc(ctx, 1, {}));
    StackVar v(ctx, gb(ctx), 8);
    if (sc(ctx, 871, {1, pid, v.addr()}) != 0) return true;
    return (r64(ctx, v.addr()) & 0xFFFFFF) - 1 > 1;
}

// cellSysmodule's (internal ones from 0xF000), when libsail loads modules.
s32 load_module(PPUContext& ctx, u32 id)
{
    if (!loads_modules(ctx)) return lv2::CELL_OK;
    return static_cast<s32>(module_call(ctx, "cellSysmodule",
        id > 0xEFFF ? "cellSysmoduleLoadModuleInternal" : "cellSysmoduleLoadModule", {id & 0xFFFF}));
}

s32 unload_module(PPUContext& ctx, u32 id)
{
    if (!loads_modules(ctx)) return lv2::CELL_OK;
    return static_cast<s32>(module_call(ctx, "cellSysmodule",
        id > 0xEFFF ? "cellSysmoduleUnloadModuleInternal" : "cellSysmoduleUnloadModule", {id & 0xFFFF}));
}

} // namespace sail

namespace {

using namespace sail;

s32 lock(PPUContext& ctx, u32 p) { return player_lock(ctx, p); }
s32 unlock(PPUContext& ctx, u32 p) { return player_unlock(ctx, p); }
u32 alloc(PPUContext& ctx, u32 p, u32 boundary, u32 size) { return player_alloc(ctx, p, boundary, size); }
void dealloc(PPUContext& ctx, u32 p, u32 boundary, u32 ptr) { player_free(ctx, p, boundary, ptr); }

// ------------------------------------------------------------------------------------------------- descriptors


s32 close_descriptor(PPUContext& ctx, u32 d, bool from_game);

// Create {stream type, media info, URI, the game's pointer}: 0x100 bytes and the elementary stream tables (each entry
// its index and the descriptor), from the player's allocator; types 0 (PAMF), 1 (MP4) and 2 (AVI) only, the memory kept
// when the type is unknown (libsail checks after allocating). The descriptor goes first on the player's list.
s32 create_descriptor(PPUContext& ctx, u32 p, u32 args)
{
    if (r8(ctx, p + pl::kState) == 0) return error(CELL_SAIL_ERROR_INVALID_STATE);
    const u32 type = r32(ctx, args), media = r32(ctx, args + 4), uri = r32(ctx, args + 8), out = r32(ctx, args + 12);
    const u32 na = r32(ctx, p + pl::kAudioCount), nv = r32(ctx, p + pl::kVideoCount), nu = r32(ctx, p + pl::kUserCount);
    const u32 size = (na + nv + nu) * 0xC0 + 0x100;
    const u32 d = alloc(ctx, p, 8, size);
    if (!d) return error(CELL_SAIL_ERROR_MEMORY);
    std::memset(gb(ctx) + d, 0, size);
    u32 es = d + 0x100;
    const auto table = [&](u32 count, u32 slot, u32 stride) {
        if (!count) return;
        w32(ctx, d + slot, es);
        for (u32 i = 0; i < count; i++) {
            w8(ctx, es + i * stride + 1, static_cast<u8>(i));
            w32(ctx, es + i * stride + 8, d);
        }
        es += count * stride;
    };
    table(na, ds::kAudio, ds::kAudioStride);
    table(nv, ds::kVideo, ds::kVideoStride);
    table(nu, ds::kUser, ds::kUserStride);
    if (type > 2) return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    w32(ctx, d + ds::kHeader, type == 0 ? 1 : 0x80 + type);
    w32(ctx, d + ds::kNext, r32(ctx, p + kDescriptors));
    w32(ctx, d + ds::kType, type);
    w32(ctx, p + kDescriptors, d);
    w32(ctx, d + ds::kUri, uri);
    w32(ctx, d + ds::kMediaInfo, media);
    w32(ctx, out, d);
    w32(ctx, d + ds::kPlayer, p);
    return lv2::CELL_OK;
}

// A descriptor freed (libsail's): an open one closed first, then state 6 and back to the allocator.
s32 free_descriptor(PPUContext& ctx, u32 p, u32 d)
{
    const u8 st = r8(ctx, d + ds::kState);
    if (st == 1 || st == 3) close_descriptor(ctx, d, false);
    w8(ctx, d + ds::kState, 6);
    dealloc(ctx, p, 8, d);
    return lv2::CELL_OK;
}

// Add: the first descriptor becomes the current one, the second the next (not the current one again); more are refused.
// Created ones become 2, open ones 3. The count of descriptors held.
s32 add_descriptor(PPUContext& ctx, u32 p, u32 d)
{
    const u32 count = r32(ctx, p + pl::kDescriptorCount);
    s32 r;
    if (count == 0) {
        w32(ctx, p + pl::kCurrentDescriptor, d);
        w32(ctx, p + pl::kDescriptorCount, 1);
        r = 1;
    } else if (count == 1) {
        if (r32(ctx, p + pl::kCurrentDescriptor) == d) return error(CELL_SAIL_ERROR_INVALID_ARG);
        w32(ctx, p + kNextDescriptor, d);
        w32(ctx, p + pl::kDescriptorCount, 2);
        r = 2;
    } else {
        return error(CELL_SAIL_ERROR_FULLED);
    }
    const u8 st = r8(ctx, d + ds::kState);
    if (st == 0) w8(ctx, d + ds::kState, 2);
    else if (st == 1) w8(ctx, d + ds::kState, 3);
    return r;
}

// Remove: the last one added (not while in use, state 5) back to created (from 2) or open, given to the game. The count
// left.
s32 remove_descriptor(PPUContext& ctx, u32 p, u32 out)
{
    const u32 count = r32(ctx, p + pl::kDescriptorCount);
    if (count == 0) return error(CELL_SAIL_ERROR_EMPTY);
    if (count > 2) return error(CELL_SAIL_ERROR_FATAL);
    const u32 slot = count == 1 ? pl::kCurrentDescriptor : kNextDescriptor;
    const u32 d = r32(ctx, p + slot);
    if (r8(ctx, d + ds::kState) == 5) return error(CELL_SAIL_ERROR_USING);
    w32(ctx, p + slot, 0);
    w32(ctx, p + pl::kDescriptorCount, count - 1);
    w8(ctx, d + ds::kState, r8(ctx, d + ds::kState) == 2 ? 0 : 1);
    if (out) w32(ctx, out, d);
    return static_cast<s32>(count - 1);
}

// Destroy: off the player's list and freed when it is there; a failure is an error event.
void destroy_descriptor(PPUContext& ctx, u32 p, u32 d)
{
    for (u32 link = p + kDescriptors, cur; (cur = r32(ctx, link)) != 0; link = cur + ds::kNext) {
        if (cur != d) continue;
        w32(ctx, link, r32(ctx, d + ds::kNext));
        if (const s32 r = free_descriptor(ctx, p, d); r < 0) notify_error(ctx, p, r);
        return;
    }
}

// ------------------------------------------------------------------------------------------------- sources

// Makeup(arg, protocol) when the source has one, the player kept first.
s32 source_makeup(PPUContext& ctx, u32 source, u32 protocol, u32 p)
{
    w32(ctx, source + src::kPlayer, p);
    const u32 f = source_func(ctx, source, src::kMakeup);
    return f ? static_cast<s32>(call_guest(ctx, f, {r32(ctx, source + src::kArg), protocol})) : 0;
}

s32 source_cleanup(PPUContext& ctx, u32 source)
{
    const u32 f = source_func(ctx, source, src::kCleanup);
    return f ? static_cast<s32>(call_guest(ctx, f, {r32(ctx, source + src::kArg)})) : 0;
}

// ReadSync(arg, stream type, media info, URI, offset, buffer, size, u64* total size).
s32 source_read_sync(PPUContext& ctx, u32 source, s32 type, u32 media, u32 uri, u64 offset, u32 buf, u32 size,
    u32 total)
{
    return static_cast<s32>(call_guest(ctx, source_func(ctx, source, src::kReadSync),
        {r32(ctx, source + src::kArg), static_cast<u64>(static_cast<s64>(type)), media, uri, offset, buf, size, total}));
}

// ------------------------------------------------------------------------------------------------- descriptors' streams

// The player's memory through its thread (messages 0x3C and 0x3D), as libsail asks for it from the game's thread.
u32 alloc_through_thread(PPUContext& ctx, u32 p, u32 boundary, u32 size)
{
    StackVar args(ctx, gb(ctx), 16);
    w32(ctx, args.addr(), boundary);
    w32(ctx, args.addr() + 4, size);
    StackVar reply(ctx, gb(ctx), 16);
    if (command(ctx, p, 0x3C, args.addr(), reply.addr()) < 0) return 0;
    if (static_cast<s32>(r32(ctx, reply.addr())) < 0) return 0;
    return r32(ctx, args.addr() + 8);
}

void free_through_thread(PPUContext& ctx, u32 p, u32 boundary, u32 ptr) { post_urgent(ctx, p, 0x3D, 0, boundary, ptr); }

void unsupported_container(PPUContext& ctx, u32 d)
{
    log_call_site(ctx, gb(ctx), std::format("cellSail: descriptor {:#x} of stream type {} (MP4 or AVI): the firmware "
        "libraries' part is not pspocket's yet", d, r32(ctx, d + ds::kType)));
    PS_FATAL(kChan, "libsail's MP4 and AVI descriptors are a later part of D-562");
}

// Message 0x3E, the control thread's side of Open: a PAMF descriptor's URI ("protocol://...") finds its registered
// source by the protocol's name (compared over the URI's protocol's length, as libsail does); the source's first user
// makes it up. libpamf loaded for the descriptor (module 0x12, unloaded again on failure).
s32 open_stream(PPUContext& ctx, u32 d)
{
    const u32 p = r32(ctx, d + ds::kPlayer);
    if (r32(ctx, d + ds::kType) != 0) unsupported_container(ctx, d);
    if (const s32 r = load_module(ctx, 0x12); r < 0) return r;
    const u8* b = gb(ctx);
    const u32 uri = r32(ctx, d + ds::kUri);
    u32 colon = 0;
    if (uri)
        for (u32 at = uri; b[at]; at++)
            if (b[at] == ':') {
                colon = at;
                break;
            }
    if (colon && b[colon + 1] == '/' && b[colon + 2] == '/') {
        const u32 n = colon - uri;
        for (u32 i = 0; i < r32(ctx, p + pl::kProtocolCount); i++) {
            const u32 name = p + pl::kProtocols + 0x20 * i;
            u32 k = 0;
            while (k < n && b[name + k] == b[uri + k] && b[name + k]) k++;
            if (k != n && b[name + k] != b[uri + k]) continue;
            const u32 source = r32(ctx, p + pl::kProtocolSources + 4 * i);
            const u32 users = r32(ctx, source + src::kUsers) + 1;
            w32(ctx, source + src::kUsers, users);
            if (users == 1) {
                if (const s32 r = source_makeup(ctx, source, name, p); r < 0) {
                    w32(ctx, source + src::kUsers, 0);
                    unload_module(ctx, 0x12);
                    return r;
                }
            }
            w32(ctx, d + ds::kProtocol, name);
            w32(ctx, d + ds::kModule, 0x12);
            w32(ctx, d + ds::kSource, source);
            return lv2::CELL_OK;
        }
    }
    unload_module(ctx, 0x12);
    return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
}

// Message 0x3F (or the control thread itself): the source's last user cleans it up; the descriptor's module unloaded.
s32 close_stream(PPUContext& ctx, u32 d)
{
    if (r32(ctx, d + ds::kType) != 0) unsupported_container(ctx, d);
    const u32 source = r32(ctx, d + ds::kSource);
    PS_CHECK(source != 0);
    const s32 users = static_cast<s32>(r32(ctx, source + src::kUsers));
    if (users > 0) {
        w32(ctx, source + src::kUsers, static_cast<u32>(users - 1));
        if (users == 1) source_cleanup(ctx, source);
    }
    const u32 module = r32(ctx, d + ds::kModule);
    w32(ctx, d + ds::kModule, 0);
    w32(ctx, d + ds::kSource, 0);
    w32(ctx, d + ds::kProtocol, 0);
    if (module) unload_module(ctx, module & 0xFFFF);
    return lv2::CELL_OK;
}

// The elementary streams chosen when the descriptor opens (each kind's first ones, in order), unless the game turned
// auto selection off; what each answers is not looked at.
void select_streams(PPUContext& ctx, u32 d)
{
    if (r8(ctx, d + ds::kNoAuto)) return;
    const u32 p = r32(ctx, d + ds::kPlayer);
    for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) select_es(ctx, d, 0, i, 0);
    for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) select_es(ctx, d, 1, i, 0);
    for (u32 i = 0; i < r32(ctx, p + pl::kUserCount); i++) select_es(ctx, d, 2, i, 0);
}

// Close (libsail's): an open descriptor back to created, a held one (from the control thread only) back to added; the
// stream closed on the control thread (a command from any other) and the header's buffer freed.
s32 close_descriptor(PPUContext& ctx, u32 d, bool from_game)
{
    const u8 st = r8(ctx, d + ds::kState);
    if (st == 1) {
        w8(ctx, d + ds::kState, 0);
    } else if (st == 0) {
        return error(CELL_SAIL_ERROR_INVALID_STATE);
    } else if (st < 6) {
        if (from_game) return error(CELL_SAIL_ERROR_INVALID_STATE);
        w8(ctx, d + ds::kState, 2);
    }
    const u32 type = r32(ctx, d + ds::kType), p = r32(ctx, d + ds::kPlayer);
    if ((type == 1 || type == 2) && r32(ctx, d + ds::kSource)) unsupported_container(ctx, d);
    const u32 lib = r32(ctx, d + ds::kSource), buf = r32(ctx, d + ds::kBuffer);
    w32(ctx, d + ds::kHeaderSize, 0);
    w32(ctx, d + ds::kBuffer, 0);
    if (lib) {
        StackVar tid(ctx, gb(ctx), 8);
        const s32 r = static_cast<s32>(lv2_lib(ctx, "sys_ppu_thread_get_id", {tid.addr()}));
        if ((r < 0 || r64(ctx, p + pl::kThread) != r64(ctx, tid.addr())) && from_game) {
            if (const s32 c = command_answer(ctx, p, 0x3F, d); c < 0) return c;
        } else {
            if (const s32 c = close_stream(ctx, d); c < 0) return c;
            from_game = false;
        }
    }
    if (!buf) return lv2::CELL_OK;
    if (from_game) free_through_thread(ctx, p, 0x80, buf);
    else dealloc(ctx, p, 0x80, buf);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- play settings

constexpr u32 kPausedNow = 0x1C9, kPauseShown = 0x166;

// Message 0x44: paused (any nonzero) kept; unpausing also clears +0x166. A change tells every renderer (bit 3 of its
// +0x34) and AU receiver (+0x48 of its callbacks' block). The answer is the paused state; `changed` for event 7.
s32 set_paused(PPUContext& ctx, u32 p, u32 arg, bool& changed)
{
    const u8 paused = arg != 0 ? 1 : 0;
    changed = paused != r8(ctx, p + kPausedNow);
    w8(ctx, p + kPausedNow, paused);
    if (!paused) w8(ctx, p + kPauseShown, 0);
    s32 result = paused;
    if (!changed) return result;
    const auto flag = [&](u32 r) { w32(ctx, r + 0x34, (paused ? 8u : 0u) | (r32(ctx, r + 0x34) & ~8u)); };
    for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) flag(r32(ctx, audio_slot(ctx, p, i) + 0x150));
    for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) flag(r32(ctx, video_slot(ctx, p, i) + 0x180));
    if (const u32 nu = r32(ctx, p + pl::kUserCount)) {
        for (u32 i = 0; i < nu; i++) w8(ctx, r32(ctx, user_slot(ctx, p, i) + 0x80) + 0x48, paused);
        result = r8(ctx, p + kPausedNow);
    }
    return result;
}

// Message 0x45 {mode, info}: modes 0-2, the 0x28 bytes of info kept when given (+0x1C8 says so).
s32 set_repeat_mode(PPUContext& ctx, u32 p, u32 args)
{
    const u32 mode = r32(ctx, args), info = r32(ctx, args + 4);
    if (mode >= 3) return error(CELL_SAIL_ERROR_INVALID_ARG);
    if (info) {
        w8(ctx, p + pl::kRepeatSet, 1);
        std::memmove(gb(ctx) + p + pl::kRepeatInfo, gb(ctx) + info, 0x28);
    } else {
        w8(ctx, p + pl::kRepeatSet, 0);
    }
    w32(ctx, p + pl::kRepeatMode, mode);
    return static_cast<s32>(mode);
}

// Messages 0x46 and 0x47 {index, muted}: bit 0 of the stream's renderer's +0x34.
void set_muted(PPUContext& ctx, u32 renderer, u8 muted)
{
    w32(ctx, renderer + 0x34, (r32(ctx, renderer + 0x34) & ~1u) | muted);
}

} // namespace

namespace sail {

// ------------------------------------------------------------------------------------------------- source sessions

// Open (libsail's): the source told the stream (type, media info, URI and a profile: 2 items, the mux rate bound, the
// duration, the size), opening (state 2) until it says the call completed.
void source_open(PPUContext& ctx, u32 s, u32 d, u64 handle)
{
    w64(ctx, s + src::kHandle, handle);
    w32(ctx, s + src::kDrain, 0);
    StackVar profile(ctx, gb(ctx), 0x20);
    w32(ctx, profile.addr() + 4, 2);
    w32(ctx, profile.addr() + 8, r32(ctx, d + ds::kMuxRate));
    w64(ctx, profile.addr() + 16, r64(ctx, d + ds::kDuration));
    w64(ctx, profile.addr() + 24, r64(ctx, d + ds::kStreamSize));
    w32(ctx, s + src::kState, 2);
    call_guest(ctx, source_func(ctx, s, src::kOpen), {r32(ctx, s + src::kArg),
        static_cast<u64>(static_cast<s64>(static_cast<s32>(r32(ctx, d + ds::kType)))), r32(ctx, d + ds::kMediaInfo),
        r32(ctx, d + ds::kUri), profile.addr()});
}

// Close (libsail's): closing (state 7) until the source says so.
void source_close(PPUContext& ctx, u32 s, u64 handle)
{
    w64(ctx, s + src::kHandle, handle);
    w32(ctx, s + src::kState, 7);
    call_guest(ctx, source_func(ctx, s, src::kClose), {r32(ctx, s + src::kArg)});
}

// Message 0x29 {source, result}: a source's call completed (without a pending read): opening becomes open (3) or closed
// (1), closing closed, starting (4) started (5) or back to open; open and stopping (6) sources keep the result (+0x50;
// stopping with no reads out completes, open, and clears the source).
void source_call_completed(PPUContext& ctx, u32 s, s32 r)
{
    const u32 st = r32(ctx, s + src::kState);
    const u32 p = r32(ctx, s + src::kPlayer);
    switch (st) {
    case 2: w32(ctx, s + src::kState, r < 0 ? 1 : 3); break;
    case 7: w32(ctx, s + src::kState, 1); break;
    case 4: w32(ctx, s + src::kState, r < 0 ? 3 : 5); break;
    case 3:
    case 6:
        if (st != 6 || r32(ctx, s + src::kOut)) {
            w32(ctx, s + src::kResult, static_cast<u32>(r));
            if (st == 6) w32(ctx, s + src::kState, 10);
            return;
        }
        w32(ctx, s + src::kState, 3);
        post_completion(ctx, p, r, r64(ctx, s + src::kHandle));
        call_guest(ctx, source_func(ctx, s, src::kClear), {r32(ctx, s + src::kArg)});
        return;
    default: return;
    }
    post_completion(ctx, p, r, r64(ctx, s + src::kHandle));
}

// ------------------------------------------------------------------------------------------------- the AV clock

// The clock (200 bytes at the player's +8): a stream opened resets what it measured.
void clock_reset(PPUContext& ctx, u32 c)
{
    w16(ctx, c + 0x8E, 0);
    w16(ctx, c + 0x88, 0);
    w16(ctx, c + 0x8A, 0);
    w32(ctx, c + 0xA0, 0xFFFFFFFF);
    w64(ctx, c + 0x78, ~u64{0});
    w32(ctx, c + 0xA4, 0);
    w32(ctx, c + 0xA8, 0);
    w32(ctx, c + 0x94, 0xFFFFFFFF);
    w32(ctx, c + 0x98, 0xFFFFFFFF);
    w32(ctx, c + 0x9C, 0xFFFFFFFF);
    w64(ctx, c + 0x70, ~u64{0});
    w16(ctx, c + 0x8C, 0);
}

// Its play state reset (libsail's): the frames counted, the times and sessions forgotten.
void clock_reset_play(PPUContext& ctx, u32 c)
{
    w16(ctx, c + 0x8A, 0);
    w32(ctx, c + 0xA0, 0xFFFFFFFF);
    w64(ctx, c + 0x78, ~u64{0});
    w32(ctx, c + 0xA4, 0);
    w32(ctx, c + 0xA8, 0);
    w32(ctx, c + 0x94, 0xFFFFFFFF);
    w32(ctx, c + 0x98, 0xFFFFFFFF);
    w32(ctx, c + 0x9C, 0xFFFFFFFF);
    w64(ctx, c + 0x70, ~u64{0});
    w16(ctx, c + 0x8C, 0);
}

// Its ticks per millisecond from the play rate (libsail's): |rate| x 90 in double precision, truncated (from 2^63 on
// through the sign bit, as the PPU converts), 1 when that is 0.
void clock_set_rate(PPUContext& ctx, u32 c, float rate)
{
    const double v = std::fabs(static_cast<double>(rate)) * 90.0;
    u64 t;
    if (std::isnan(v)) t = 0x8000000000000000ull; // fctidz of a NaN
    else if (v >= 9223372036854775808.0) {
        const double w = v - 9223372036854775808.0;
        t = (w >= 9223372036854775808.0 ? 0x7FFFFFFFFFFFFFFFull : static_cast<u64>(static_cast<s64>(w))) ^
            0x8000000000000000ull;
    } else {
        t = static_cast<u64>(static_cast<s64>(v));
    }
    w64(ctx, c, t != 0 ? t : 1);
}

// The time it holds (libsail's): the video half's once a video frame was counted, else the audio half's.
u64 clock_time(PPUContext& ctx, u32 c)
{
    return static_cast<s16>(ps_read16(gb(ctx), c + 0x8E)) > 0 ? r64(ctx, c + 0x68) : r64(ctx, c + 0x58);
}

// The clock running or not (+0x90), under its lwmutex; running needs a frame seen (+0x8E) and forgets the last time.
u32 clock_run(PPUContext& ctx, u32 c, u32 run)
{
    lv2_lib(ctx, "sys_lwmutex_lock", {c + 0xB0, 0});
    u32 now = r32(ctx, c + 0x90);
    if ((now != 0 ? 1u : 0u) != run) {
        now = 0;
        bool set = true;
        if (run) {
            if (ps_read16(gb(ctx), c + 0x8E) == 0) set = false;
            else {
                now = 1;
                w32(ctx, c + 0x94, 0xFFFFFFFF);
                w64(ctx, c + 0x70, ~u64{0});
            }
        }
        if (set) w32(ctx, c + 0x90, now);
    }
    lv2_lib(ctx, "sys_lwmutex_unlock", {c + 0xB0});
    return now;
}

} // namespace sail

namespace {

// ------------------------------------------------------------------------------------------------- streams

// A step's handle: the message its completion becomes (code << 32) and its record. Records: +0 flags (0x20 in use, 1
// failed), +4 the steps still out, +8 the handle of the step it belongs to.
constexpr u32 kStreamPhase = 0x160, kRateShown = 0x162; // 1 opening, 2 open, 7 closing, 0 none
constexpr u32 kCallRunning = 0x250;

// A source's Read (libsail's): the call's handle kept for the source's NotifyCallCompleted to complete (+0x60), then its
// read(arg, stream type, media info, URI, offset, buffer, size, u64* total size).
s32 source_read(PPUContext& ctx, u32 source, s32 type, u32 media, u32 uri, u64 offset, u32 buf, u32 size, u32 total,
    u64 handle)
{
    w64(ctx, source + 0x60, handle);
    return static_cast<s32>(call_guest(ctx, source_func(ctx, source, src::kRead),
        {r32(ctx, source + src::kArg), static_cast<u64>(static_cast<s64>(type)), media, uri, offset, buf, size, total}));
}

// A descriptor's header read through its source (libsail's): the buffer it had freed, `size` bytes asked for into a
// new one from the player's allocator (MEMORY without), the source's Read completing message 0x4A on `rec` (+4 the
// descriptor); the buffer and the size asked kept (+0xC8, +0xCC) once the read is under way, freed when it is refused.
// Its total size goes to a stack word of libsail's that is gone by then.
s32 read_header(PPUContext& ctx, u32 d, u32 size, u32 rec)
{
    w32(ctx, rec + 4, d);
    const u32 type = r32(ctx, d + ds::kType);
    if (type == 1 || type == 2) unsupported_container(ctx, d);
    if (type != 0) return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    const u32 p = r32(ctx, d + ds::kPlayer), old = r32(ctx, d + ds::kBuffer);
    w32(ctx, d + ds::kBuffer, 0);
    if (old) player_free(ctx, p, 0x80, old);
    const u32 buf = player_alloc(ctx, p, 0x80, size);
    if (!buf) return error(CELL_SAIL_ERROR_MEMORY);
    s32 r;
    {
        StackVar total(ctx, gb(ctx), 16);
        r = source_read(ctx, r32(ctx, d + ds::kSource), static_cast<s32>(type), r32(ctx, d + ds::kMediaInfo),
            r32(ctx, d + ds::kUri), 0, buf, size, total.addr(), step(0x4A, rec));
    }
    if (r < 0) {
        player_free(ctx, p, 0x80, buf);
        return r;
    }
    w32(ctx, d + ds::kHeaderSize, size);
    w32(ctx, d + ds::kBuffer, buf);
    return r;
}

// The header read (message 0x4A, libsail's): a failed read closes the descriptor's stream (back to added) and fails
// the preparation; a header longer than what was read is read again whole; else it is parsed (the stream's size, a
// stream without entry points flagged (+0x18 bit 0), the mux rate, the duration), the streams chosen, the descriptor in
// use (5). A failure after the read frees the buffer (the old one again when a second read was refused, as libsail).
void header_read(PPUContext& ctx, u32 p, s32 result, u32 rec)
{
    const u32 d = r32(ctx, rec + 4);
    if (result < 0) {
        close_descriptor(ctx, d, false);
        post_completion(ctx, p, result, r64(ctx, rec + 8));
        return;
    }
    const u32 type = r32(ctx, d + ds::kType);
    if (type != 0) unsupported_container(ctx, d);
    const u32 rd = d + ds::kReader, buf = r32(ctx, d + ds::kBuffer);
    StackVar out(ctx, gb(ctx), 0x40); // +0 the header's size, +0x10 the stream's, +0x20 / +0x30 the start and end
    s32 r = static_cast<s32>(module_call(ctx, "cellPamf", "cellPamfGetStreamOffsetAndSize",
        {buf, 0, out.addr(), out.addr() + 0x10}));
    if (r >= 0) {
        const u64 header = r64(ctx, out.addr());
        if (u64{r32(ctx, d + ds::kHeaderSize)} < header) {
            r = read_header(ctx, d, static_cast<u32>(header), rec);
            if (r >= 0) return;
        } else {
            w32(ctx, d + ds::kHeaderSize, static_cast<u32>(header));
            w64(ctx, d + ds::kStreamSize, r64(ctx, out.addr() + 0x10));
            r = static_cast<s32>(module_call(ctx, "cellPamf", "cellPamfReaderInitialize", {rd, buf, 0, 1}));
            if (r >= 0) {
                if (static_cast<s32>(module_call(ctx, "cellPamf", "cellPamfReaderGetNumberOfEp", {rd})) == 0)
                    w32(ctx, d + 0x18, r32(ctx, d + 0x18) | 1);
                const u32 mux = static_cast<u32>(module_call(ctx, "cellPamf", "cellPamfReaderGetMuxRateBound", {rd}));
                w32(ctx, d + ds::kMuxRate, mux * 400);
                const u32 start = out.addr() + 0x20, end = out.addr() + 0x30;
                module_call(ctx, "cellPamf", "cellPamfReaderGetPresentationStartTime", {rd, start});
                module_call(ctx, "cellPamf", "cellPamfReaderGetPresentationEndTime", {rd, end});
                w64(ctx, d + ds::kDuration, u64{r32(ctx, end) - r32(ctx, start)} << 32 |
                    (r32(ctx, end + 4) - r32(ctx, start + 4)));
                select_streams(ctx, d);
                w8(ctx, d + ds::kState, 5);
                post_completion(ctx, p, 0, r64(ctx, rec + 8));
                return;
            }
        }
    }
    if (buf) {
        player_free(ctx, p, 0x80, buf);
        w32(ctx, d + ds::kHeaderSize, 0);
        w32(ctx, d + ds::kBuffer, 0);
    }
    post_completion(ctx, p, r, r64(ctx, rec + 8));
}

// The current descriptor's stream made ready for OpenStream (libsail's): one opened already (3) has its streams chosen
// and is in use (5); one only added (2) is opened (4: its source found and made up) and its header read through the
// source (message 0x4A goes on; a refused read closes the stream again, the descriptor left opening); others complete
// at once.
void stream_prepare(PPUContext& ctx, u32 p, u32 d, u32 rec, u64 handle)
{
    std::memset(gb(ctx) + rec, 0, 0x20);
    w64(ctx, rec + 8, handle);
    w32(ctx, rec + 4, d);
    w32(ctx, rec, r32(ctx, rec) | 0x20);
    const u8 st = r8(ctx, d + ds::kState);
    if (st >= 6) {
        post_completion(ctx, p, error(CELL_SAIL_ERROR_FATAL), handle);
        return;
    }
    if (st == 2) {
        w8(ctx, d + ds::kState, 4);
        s32 r = open_stream(ctx, d);
        if (r >= 0) {
            r = read_header(ctx, d, 0x800, rec);
            if (r >= 0) return;
            close_stream(ctx, d);
        }
        post_completion(ctx, p, r, handle);
        return;
    }
    if (st == 3) {
        select_streams(ctx, d);
        w8(ctx, d + ds::kState, 5);
    }
    post_completion(ctx, p, 0, handle);
}

} // namespace

namespace sail {

// The media state (+0x162, libsail's): a change is event 11 (the value, the descriptor) and tells each chosen stream's
// renderer (flag 0x10) or AU receiver (+0x4A of its object).
void set_media_state(PPUContext& ctx, u32 p, u32 value, u32 d)
{
    if (r8(ctx, p + 0x162) == value) return;
    notify_event(ctx, p, 11, value, d);
    w8(ctx, p + 0x162, static_cast<u8>(value));
    const u32 on = value != 0 ? 0x10 : 0;
    for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) {
        if (!r8(ctx, r32(ctx, d + ds::kAudio) + i * ds::kAudioStride)) continue;
        const u32 r = r32(ctx, audio_slot(ctx, p, i) + as::kRenderer);
        w32(ctx, r + 0x34, on | (r32(ctx, r + 0x34) & ~0x10u));
    }
    for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) {
        if (!r8(ctx, r32(ctx, d + ds::kVideo) + i * ds::kVideoStride)) continue;
        const u32 r = r32(ctx, video_slot(ctx, p, i) + vs::kRenderer);
        w32(ctx, r + 0x34, on | (r32(ctx, r + 0x34) & ~0x10u));
    }
    for (u32 i = 0; i < r32(ctx, p + pl::kUserCount); i++) {
        if (!r8(ctx, r32(ctx, d + ds::kUser) + i * ds::kUserStride)) continue;
        w8(ctx, r32(ctx, user_slot(ctx, p, i) + us::kReceiver) + 0x4A, value != 0 ? 1 : 0);
    }
}

} // namespace sail

namespace {

// The stream taken down (libsail's): its source closed (message 0x5F), its open elementary streams (0x61-0x63) and the
// dmux pipeline (0x60), the record counting them.
void stream_close(PPUContext& ctx, u32 p, u32 d, u32 rec, u64 handle)
{
    std::memset(gb(ctx) + rec, 0, 0x10);
    w32(ctx, rec, r32(ctx, rec) | 0x20);
    w64(ctx, rec + 8, handle);
    w8(ctx, p + kStreamPhase, 7);
    set_media_state(ctx, p, 0, d);
    u32 steps = 0;
    if (const u32 s = r32(ctx, d + ds::kSource)) {
        const u32 type = r32(ctx, d + ds::kType);
        if (type == 1 || type == 2) unsupported_container(ctx, d);
        steps = 1;
        source_close(ctx, s, step(0x5F, rec));
    }
    // Each open elementary stream (its entry 2) closed.
    for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++)
        if (r8(ctx, r32(ctx, d + ds::kAudio) + i * ds::kAudioStride) == 2) {
            steps++;
            es_close_audio(ctx, p, audio_slot(ctx, p, i), step(0x61, rec), false);
        }
    for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++)
        if (r8(ctx, r32(ctx, d + ds::kVideo) + i * ds::kVideoStride) == 2) {
            steps++;
            es_close_video(ctx, p, video_slot(ctx, p, i), step(0x62, rec), false);
        }
    for (u32 i = 0; i < r32(ctx, p + pl::kUserCount); i++)
        if (r8(ctx, r32(ctx, d + ds::kUser) + i * ds::kUserStride) == 2) {
            steps++;
            es_close_user(ctx, p, user_slot(ctx, p, i), step(0x63, rec), false);
        }
    dmux_close(ctx, p + pl::kDmux, step(0x60, rec));
    w32(ctx, rec + 4, (steps + 1) & 0xFF);
}

// Message 0x4E: the descriptor ready, the stream opened: the source (0x4F), the dmux pipeline (0x50) and each chosen
// elementary stream (0x51-0x53), the record counting them, the clock reset. A failure closes it all (0x5C).
void stream_open_steps(PPUContext& ctx, u32 p, s32 result, u32 rec)
{
    const u32 d = r32(ctx, p + pl::kCurrentDescriptor);
    if (result < 0) {
        w32(ctx, rec, r32(ctx, rec) | 1);
        notify_error(ctx, p, result);
        w8(ctx, d + ds::kState, 2);
        stream_close(ctx, p, d, rec + 0x10, step(0x5C, rec));
        close_descriptor(ctx, d, false);
        return;
    }
    w8(ctx, d + ds::kState, 5);
    w8(ctx, p + kStreamPhase, 1);
    const u32 type = r32(ctx, d + ds::kType);
    if (type == 1 || type == 2) unsupported_container(ctx, d);
    const u32 s = r32(ctx, d + ds::kSource);
    source_open(ctx, s, d, step(0x4F, rec));
    dmux_open(ctx, p + pl::kDmux, d + ds::kHeader, s, step(0x50, rec));
    u32 steps = 2;
    // Each chosen elementary stream (its entry 1) opened.
    for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) {
        const u32 e = r32(ctx, d + ds::kAudio) + i * ds::kAudioStride;
        if (r8(ctx, e) != 1) continue;
        steps++;
        es_open_audio(ctx, p, d, audio_slot(ctx, p, i), e, step(0x51, rec));
    }
    for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) {
        const u32 e = r32(ctx, d + ds::kVideo) + i * ds::kVideoStride;
        if (r8(ctx, e) != 1) continue;
        steps++;
        es_open_video(ctx, p, d, video_slot(ctx, p, i), e, step(0x52, rec));
    }
    for (u32 i = 0; i < r32(ctx, p + pl::kUserCount); i++) {
        const u32 e = r32(ctx, d + ds::kUser) + i * ds::kUserStride;
        if (r8(ctx, e) != 1) continue;
        steps++;
        es_open_user(ctx, p, d, user_slot(ctx, p, i), e, step(0x53, rec));
    }
    w32(ctx, rec + 4, steps & 0xFF);
    clock_reset(ctx, p + pl::kSync);
}

// Messages 0x4F-0x53: one opening step done. A failure marks the record; when none is out the stream is open (+0x160
// 2): a failure closes it again (0x5C), else event 4 with the descriptor and the open completes.
void stream_open_counted(PPUContext& ctx, u32 p, s32 result, u32 rec)
{
    if (result < 0) {
        w32(ctx, rec, r32(ctx, rec) | 1);
        notify_error(ctx, p, result);
    }
    const u32 left = r32(ctx, rec + 4) - 1;
    w32(ctx, rec + 4, left);
    if (left) return;
    w8(ctx, p + kStreamPhase, 2);
    const u32 d = r32(ctx, p + pl::kCurrentDescriptor);
    if (r32(ctx, rec) & 1) {
        stream_close(ctx, p, d, rec + 0x10, step(0x5C, rec));
        return;
    }
    notify_event(ctx, p, 4, d, 0);
    post_completion(ctx, p, 0, r64(ctx, rec + 8));
}

// Messages 0x5F-0x63: one closing step done (a failure an error event). When none is out the descriptor leaves the
// player (in use: back to open; else created), the next becomes current, the play flags and the clock stop, event 5
// with the descriptor, and the close completes.
void stream_close_counted(PPUContext& ctx, u32 p, s32 result, u32 rec)
{
    if (result < 0) notify_error(ctx, p, result);
    const u32 left = r32(ctx, rec + 4) - 1;
    w32(ctx, rec + 4, left);
    if (left) return;
    w32(ctx, p + pl::kDescriptorCount, r32(ctx, p + pl::kDescriptorCount) - 1);
    const u32 d = r32(ctx, p + pl::kCurrentDescriptor);
    w8(ctx, d + ds::kState, r8(ctx, d + ds::kState) == 5 ? 1 : 0);
    w32(ctx, p + pl::kCurrentDescriptor, r32(ctx, p + kNextDescriptor));
    w32(ctx, p + kNextDescriptor, 0);
    w8(ctx, p + 0x164, 0);
    w8(ctx, p + 0x1CA, 0);
    w8(ctx, p + 0x165, 0);
    w8(ctx, p + 0x1CB, 0);
    clock_run(ctx, p + pl::kSync, 0);
    w8(ctx, p + kStreamPhase, 0);
    notify_event(ctx, p, 5, d, 0);
    post_completion(ctx, p, 0, r64(ctx, rec + 8));
}

// Message 0x4C {the game's u64 argument}: OpenStream, booted (2) with nothing running and a descriptor held (EMPTY
// without): state 3 and its event; then the descriptor made ready (0x4E), all of it completing 0x4D.
s32 open_stream_command(PPUContext& ctx, u32 p, u32 arg)
{
    if (r8(ctx, p + pl::kState) != 2 || r32(ctx, p + kCallRunning) != 0) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (static_cast<s32>(r32(ctx, p + pl::kDescriptorCount)) <= 0) return error(CELL_SAIL_ERROR_EMPTY);
    w32(ctx, p + kCallRunning, 2);
    w64(ctx, p + pl::kBootArg, r64(ctx, arg));
    w8(ctx, p + pl::kState, 3);
    notify_event(ctx, p, 3, 3, 0);
    return lv2::CELL_OK;
}

void open_stream_start(PPUContext& ctx, u32 p)
{
    std::memset(gb(ctx) + p + 0xD0, 0, 0x40);
    std::memset(gb(ctx) + p + 0xE0, 0, 0x30);
    w32(ctx, p + 0xE0, r32(ctx, p + 0xE0) | 0x20);
    w64(ctx, p + 0xE8, step(0x4D, p + 0xD0));
    w8(ctx, p + kRateShown, 0);
    stream_prepare(ctx, p, r32(ctx, p + pl::kCurrentDescriptor), p + 0xF0, step(0x4E, p + 0xE0));
}

// Message 0x5D {the game's u64 argument}: CloseStream, open (4) with nothing running: state 8 and its event; then the
// stream taken down, completing 0x5E.
s32 close_stream_command(PPUContext& ctx, u32 p, u32 arg)
{
    if (r8(ctx, p + pl::kState) != 4 || r32(ctx, p + kCallRunning) != 0) return error(CELL_SAIL_ERROR_INVALID_STATE);
    w32(ctx, p + kCallRunning, 3);
    w64(ctx, p + pl::kBootArg, r64(ctx, arg));
    w8(ctx, p + pl::kState, 8);
    notify_event(ctx, p, 3, 8, 0);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- the message loop

struct Message {
    u32 code, a, b, c;
};

Message receive(PPUContext& ctx, u32 p, u32 last)
{
    const u32 qa = p + pl::kQueue;
    w32(ctx, p + kMessage, 200);
    Message m{last, 0, 0, 0};
    if (sc(ctx, 102, {r32(ctx, qa + q::kMutex), 0}) >= 0) {
        while (r32(ctx, qa + q::kCount) == 0) sc(ctx, 107, {r32(ctx, qa + q::kCond), 3000000});
        const u32 read = r32(ctx, qa + q::kRead), at = r32(ctx, qa + q::kRing) + 16 * read;
        m = Message{r32(ctx, at), r32(ctx, at + 4), r32(ctx, at + 8), r32(ctx, at + 12)};
        const u32 cap = r32(ctx, qa + q::kCapacity);
        w32(ctx, qa + q::kRead, cap ? (read + 1) % cap : 0);
        w32(ctx, qa + q::kCount, r32(ctx, qa + q::kCount) - 1);
        sc(ctx, 108, {r32(ctx, qa + q::kCond)});
        sc(ctx, 104, {r32(ctx, qa + q::kMutex)});
    }
    w32(ctx, p + kMessage, m.code);
    return m;
}

// Runs messages until stop. Each handler is libsail's case for the message's code (its low 16 bits).
void message_loop(PPUContext& ctx, u32 p)
{
    u32 last = 0;
    for (;;) {
        const Message m = receive(ctx, p, last);
        last = m.code;
        u32 code = m.code & 0xFFFF;
        if (code == 0) {
            // A call's completion {0, result, handle}: handled as the call its handle names.
            code = m.b & 0xFFFF;
            last = code;
        }
        if (code == 0x3D) {
            dealloc(ctx, p, m.b, m.c); // memory freed for the game's thread
            continue;
        }
        if (es_message(ctx, p, code, m.a, m.b, m.c)) continue;
        if (play_message(ctx, p, code, static_cast<s32>(m.a), m.b, m.c)) continue;
        if (code == 0x3C || code == 0x3E || code == 0x3F) {
            if (const s32 r = lock(ctx, p); r < 0) {
                notify_error(ctx, p, r);
                notify_error(ctx, p, error(CELL_SAIL_ERROR_FATAL));
                continue;
            }
            if (code == 0x3C) {
                // Memory for the game's thread: {boundary, size, the memory}.
                w32(ctx, m.b + 8, alloc(ctx, p, r32(ctx, m.b), r32(ctx, m.b + 4)));
                answer(ctx, p, m.c, 0);
            } else {
                answer(ctx, p, m.c, code == 0x3E ? open_stream(ctx, m.b) : close_stream(ctx, m.b));
            }
            continue;
        }
        if (code == 0x29) {
            source_call_completed(ctx, m.b, static_cast<s32>(m.c));
            continue;
        }
        if (code == 0x4A) {
            header_read(ctx, p, static_cast<s32>(m.a), m.c);
            continue;
        }
        if (code == 0x4C || code == 0x5D) {
            if (const s32 r = lock(ctx, p); r < 0) {
                notify_error(ctx, p, r);
                notify_error(ctx, p, error(CELL_SAIL_ERROR_FATAL));
                continue;
            }
            const s32 r = code == 0x4C ? open_stream_command(ctx, p, m.b) : close_stream_command(ctx, p, m.b);
            if (!answer(ctx, p, m.c, r) || r < 0) continue;
            if (code == 0x4C) {
                open_stream_start(ctx, p);
            } else {
                std::memset(gb(ctx) + p + 0xD0, 0, 0x20);
                stream_close(ctx, p, r32(ctx, p + pl::kCurrentDescriptor), p + 0xE0, step(0x5E, p + 0xD0));
            }
            continue;
        }
        if (code == 0x4D || code == 0x5E) {
            // OpenStream / CloseStream done: open (4) or, failed, booted (2), the call's completion event.
            const s32 result = static_cast<s32>(m.a);
            if (code == 0x5E && result < 0) {
                w32(ctx, m.c, r32(ctx, m.c) | 1);
                notify_error(ctx, p, result);
            }
            const u8 st = code == 0x4D && result >= 0 ? 4 : 2;
            w8(ctx, p + pl::kState, st);
            notify_event(ctx, p, 3, st, 0);
            call_completed(ctx, p, code == 0x4D ? 2 : 3, code == 0x4D ? result : 0);
            continue;
        }
        if (code == 0x4E) {
            stream_open_steps(ctx, p, static_cast<s32>(m.a), m.c);
            continue;
        }
        if (code >= 0x4F && code <= 0x53) {
            stream_open_counted(ctx, p, static_cast<s32>(m.a), m.c);
            continue;
        }
        if (code == 0x5C) {
            post_completion(ctx, p, error(CELL_SAIL_ERROR_CANCEL), r64(ctx, m.c + 8));
            continue;
        }
        if (code >= 0x5F && code <= 0x63) {
            stream_close_counted(ctx, p, static_cast<s32>(m.a), m.c);
            continue;
        }
        if (code >= 0x44 && code <= 0x47) {
            if (const s32 r = lock(ctx, p); r < 0) {
                notify_error(ctx, p, r);
                notify_error(ctx, p, error(CELL_SAIL_ERROR_FATAL));
                continue;
            }
            if (code == 0x44) {
                bool changed = false;
                const s32 paused = set_paused(ctx, p, m.b, changed);
                if (answer(ctx, p, m.c, paused) && changed) notify_event(ctx, p, 7, static_cast<u32>(paused), 0);
            } else if (code == 0x45) {
                answer(ctx, p, m.c, set_repeat_mode(ctx, p, m.b));
            } else {
                const u32 index = r32(ctx, m.b);
                set_muted(ctx, r32(ctx, code == 0x46 ? audio_slot(ctx, p, index) + 0x150 : video_slot(ctx, p, index) + 0x180),
                    r8(ctx, m.b + 4));
                answer(ctx, p, m.c, 0);
            }
            continue;
        }
        if (code >= 0x40 && code <= 0x43) {
            // Descriptor commands, under the player's lock; a failed lock is an error and a fatal error.
            if (const s32 r = lock(ctx, p); r < 0) {
                notify_error(ctx, p, r);
                notify_error(ctx, p, error(CELL_SAIL_ERROR_FATAL));
                continue;
            }
            if (code == 0x40) answer(ctx, p, m.c, create_descriptor(ctx, p, m.b));
            else if (code == 0x42) answer(ctx, p, m.c, add_descriptor(ctx, p, m.b));
            else if (code == 0x43) answer(ctx, p, m.c, remove_descriptor(ctx, p, m.b));
            else {
                destroy_descriptor(ctx, p, m.b);
                unlock(ctx, p);
            }
            continue;
        }
        switch (code) {
        case 0xD1: return; // stop
        case 0xD2:
        case 0xD4: break;
        default:
            log_call_site(ctx, gb(ctx), std::format("cellSail: control message {:#x} ({:#x}, {:#x}, {:#x}) is not "
                "pspocket's yet", m.code, m.a, m.b, m.c));
            PS_FATAL(kChan, "libsail's control message {:#x} is a later part of D-562", m.code);
        }
    }
}

// ------------------------------------------------------------------------------------------------- the thread

void _pspocket_cellSail_control(PPUContext& ctx, u64 player)
{
    const u32 p = static_cast<u32>(player);
    w64(ctx, p + pl::kFrame, ctx.gpr[1]);
    w32(ctx, p + kBootRunning, 1);
    s32 r = lock(ctx, p);
    bool failed = r < 0;
    u32 owned = 0;
    if (failed) {
        w8(ctx, p + pl::kState, pl::kBootFailed);
    } else {
        if (!r32(ctx, p + pl::kSpurs)) {
            const u32 spurs = alloc(ctx, p, 0x80, 0x1000);
            r = error(CELL_SAIL_ERROR_MEMORY);
            failed = true;
            if (spurs) {
                owned = spurs;
                StackVar attr(ctx, gb(ctx), 0x200);
                r = static_cast<s32>(module_call(ctx, "cellSpurs", "_cellSpursAttributeInitialize",
                    {attr.addr(), 2, 0x330000, r32(ctx, p + pl::kSpursSpus),
                        static_cast<u64>(static_cast<s64>(static_cast<s32>(r32(ctx, p + pl::kSpursSpuPrio)))),
                        static_cast<u64>(static_cast<s64>(static_cast<s32>(r32(ctx, p + pl::kSpursPpuPrio)))),
                        r8(ctx, p + pl::kSpursExit)}));
                if (r >= 0) {
                    StackVar prefix(ctx, gb(ctx), 16);
                    std::memcpy(gb(ctx) + prefix.addr(), "_libsail", 8);
                    r = static_cast<s32>(module_call(ctx, "cellSpurs", "cellSpursAttributeSetNamePrefix",
                        {attr.addr(), prefix.addr(), 8}));
                }
                if (r >= 0) r = static_cast<s32>(module_call(ctx, "cellSpurs", "cellSpursInitializeWithAttribute",
                    {spurs, attr.addr()}));
                if (r >= 0) {
                    w32(ctx, p + pl::kSpurs, spurs);
                    failed = false;
                }
            }
        } else {
            failed = false;
        }
        if (!failed) {
            const u32 na = r32(ctx, p + pl::kAudioCount), nv = r32(ctx, p + pl::kVideoCount),
                      nu = r32(ctx, p + pl::kUserCount);
            const auto audio = [&](u32 i) { return r32(ctx, p + pl::kAudio) + i * pl::kAudioStride; };
            const auto video = [&](u32 i) { return r32(ctx, p + pl::kVideo) + i * pl::kVideoStride; };
            const auto user = [&](u32 i) { return r32(ctx, p + pl::kUser) + i * pl::kUserStride; };
            // The pipelines, then the renderers and the AU receivers: what failed undone, as libsail's set-up.
            dmux_init(ctx, p + pl::kDmux, p);
            u32 ia = 0, iv = 0;
            r = lv2::CELL_OK;
            while (r >= 0 && ia < na)
                if ((r = audio_pipeline_init(ctx, audio(ia) + 0x80, p)) >= 0) ia++;
            while (r >= 0 && iv < nv)
                if ((r = video_pipeline_init(ctx, video(iv) + 0x80, p)) >= 0) iv++;
            if (r < 0) {
                for (u32 i = 0; i < iv; i++) video_pipeline_fini(ctx, video(i) + 0x80);
                for (u32 i = 0; i < ia; i++) audio_pipeline_fini(ctx, audio(i) + 0x80);
                dmux_fini(ctx, p + pl::kDmux);
                failed = true;
            } else {
                u32 ra = 0, rv = 0, ru = 0;
                while (r >= 0 && ra < na) {
                    // A renderer of a stream in mode 1 is told so (+0x34 = 4) first.
                    const u32 rd = r32(ctx, audio(ra) + 0x150);
                    if (ps_read16(gb(ctx), audio(ra) + 0x156) == 1) w32(ctx, rd + 0x34, 4);
                    if ((r = renderer_makeup(ctx, rd)) >= 0) ra++;
                }
                while (r >= 0 && rv < nv) {
                    const u32 rd = r32(ctx, video(rv) + 0x180);
                    if (ps_read16(gb(ctx), video(rv) + 0x186) == 1) w32(ctx, rd + 0x34, 4);
                    if ((r = renderer_makeup(ctx, rd)) >= 0) rv++;
                }
                while (r >= 0 && ru < nu)
                    if ((r = receiver_makeup(ctx, r32(ctx, user(ru) + 0x80), p, p + pl::kSync, ru)) >= 0) ru++;
                if (r >= 0) {
                    w8(ctx, p + pl::kState, 2);
                    notify_event(ctx, p, 3, 2, 0);
                    notify_completed(ctx, p, 1, 0);
                    sc(ctx, 108, {r32(ctx, p + pl::kLock + 4)});
                    unlock(ctx, p);
                    message_loop(ctx, p);
                    lock(ctx, p);
                    for (u32 i = 0; i < rv; i++) video_pipeline_stop(ctx, video(i) + 0x80);
                    for (u32 i = 0; i < ra; i++) audio_pipeline_stop(ctx, audio(i) + 0x80);
                    // The descriptors the player holds back to their open state.
                    for (const u32 at : {p + pl::kCurrentDescriptor, p + kNextDescriptor}) {
                        if (const u32 d = r32(ctx, at)) {
                            const u8 st = r8(ctx, d + 0x38);
                            if (st == 2) w8(ctx, d + 0x38, 0);
                            else if (st >= 3 && st <= 5) w8(ctx, d + 0x38, 1);
                        }
                    }
                    w32(ctx, p + pl::kDescriptorCount, 0);
                } else {
                    failed = true;
                }
                for (u32 i = 0; i < ru; i++) receiver_cleanup(ctx, r32(ctx, user(i) + 0x80));
                for (u32 i = 0; i < rv; i++) renderer_cleanup(ctx, r32(ctx, video(i) + 0x180));
                for (u32 i = 0; i < ra; i++) renderer_cleanup(ctx, r32(ctx, audio(i) + 0x150));
                for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) audio_pipeline_fini(ctx, audio(i) + 0x80);
                for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) video_pipeline_fini(ctx, video(i) + 0x80);
                dmux_fini(ctx, p + pl::kDmux);
                for (u32 d = r32(ctx, p + kDescriptors); d;) {
                    const u32 next = r32(ctx, d + ds::kNext);
                    free_descriptor(ctx, p, d);
                    d = next;
                }
                w32(ctx, p + kDescriptors, 0);
            }
        }
        unlock(ctx, p);
        if (owned) {
            if (r32(ctx, p + pl::kSpurs)) {
                module_call(ctx, "cellSpurs", "cellSpursFinalize", {owned});
                w32(ctx, p + pl::kSpurs, 0);
            }
            dealloc(ctx, p, 0x80, owned);
        }
        w8(ctx, p + pl::kState, pl::kBootFailed);
    }
    if (failed) {
        notify_event(ctx, p, 3, 9, 0);
        notify_completed(ctx, p, 1, r);
    }
    lv2_lib(ctx, "sys_ppu_thread_exit", {0});
}

// ------------------------------------------------------------------------------------------------- Boot

s32 cellSailPlayerBoot(PPUContext& ctx, u32 p, u64 arg)
{
    const s32 r0 = lock(ctx, p);
    if (r0 < 0) return r0;
    s32 r = error(CELL_SAIL_ERROR_INVALID_STATE);
    bool ready = r8(ctx, p + pl::kState) == pl::kInitialized;
    for (u32 i = 0; ready && i < r32(ctx, p + pl::kAudioCount); i++)
        ready = r32(ctx, r32(ctx, p + pl::kAudio) + i * pl::kAudioStride + 0x150) != 0;
    for (u32 i = 0; ready && i < r32(ctx, p + pl::kVideoCount); i++)
        ready = r32(ctx, r32(ctx, p + pl::kVideo) + i * pl::kVideoStride + 0x180) != 0;
    for (u32 i = 0; ready && i < r32(ctx, p + pl::kUserCount); i++)
        ready = r32(ctx, r32(ctx, p + pl::kUser) + i * pl::kUserStride + 0x80) != 0;
    if (ready) {
        w64(ctx, p + pl::kBootArg, arg);
        w8(ctx, p + pl::kState, pl::kBooted);
        StackVar name(ctx, gb(ctx), 32);
        std::memcpy(gb(ctx) + name.addr(), "_libsail-control", 17);
        const u32 entry = HleProcess::current()->link_function("cellSail", recomp::nid_of("_pspocket_cellSail_control"),
            "_pspocket_cellSail_control");
        r = static_cast<s32>(lv2_lib(ctx, "sys_ppu_thread_create", {p + pl::kThread, entry, p,
            r32(ctx, p + pl::kThreadPrio), r32(ctx, p + pl::kThreadStack), 1, name.addr()}));
        if (r < 0) w8(ctx, p + pl::kState, pl::kInitialized);
    }
    unlock(ctx, p);
    return r;
}

// ------------------------------------------------------------------------------------------------- the game's calls

// Open: a created descriptor (or 4) opened; open ones answer 0. The stream found through its source on the control
// thread, then, for PAMF, the header read through the source's ReadSync into memory from the control thread (2 KB
// first, then the header's own size), libpamf's reader made over it (verified), the mux rate (x400), the stream's size
// and its duration (the presentation end less the start) kept, the elementary streams chosen, state 1. A failure frees
// the buffer and closes the stream again.
s32 cellSailDescriptorOpen(PPUContext& ctx, u32 d)
{
    const u8 st = r8(ctx, d + ds::kState);
    if (st == 1 || st == 3 || st == 5) return lv2::CELL_OK;
    if (st != 0 && st != 4) return error(CELL_SAIL_ERROR_INVALID_STATE);
    const u32 p = r32(ctx, d + ds::kPlayer);
    s32 r = command_answer(ctx, p, 0x3E, d);
    if (r < 0) return r;
    const u32 type = r32(ctx, d + ds::kType);
    if (type == 1 || type == 2) unsupported_container(ctx, d);
    bool opened = false;
    u32 buf = 0;
    if (type != 0) {
        r = error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    } else {
        const u32 old = r32(ctx, d + ds::kBuffer);
        w32(ctx, d + ds::kBuffer, 0);
        if (old) free_through_thread(ctx, p, 0x80, old);
        StackVar out(ctx, gb(ctx), 0x30); // total size, header offset, stream size, start and end times
        const u32 total = out.addr(), offset = out.addr() + 8, size = out.addr() + 16, start = out.addr() + 24,
                  end = out.addr() + 32;
        u32 want = 0x800;
        for (;;) {
            buf = alloc_through_thread(ctx, p, 0x80, want);
            if (!buf) {
                r = error(CELL_SAIL_ERROR_MEMORY);
                break;
            }
            r = source_read_sync(ctx, r32(ctx, d + ds::kSource), static_cast<s32>(type), r32(ctx, d + ds::kMediaInfo),
                r32(ctx, d + ds::kUri), 0, buf, want, total);
            if (r >= 0)
                r = static_cast<s32>(module_call(ctx, "cellPamf", "cellPamfGetStreamOffsetAndSize", {buf, 0, offset, size}));
            if (r < 0) {
                free_through_thread(ctx, p, 0x80, buf);
                break;
            }
            const u64 header = r64(ctx, offset);
            if (want >= header) {
                w32(ctx, d + ds::kHeaderSize, static_cast<u32>(header));
                w32(ctx, d + ds::kBuffer, buf);
                w64(ctx, d + ds::kStreamSize, r64(ctx, size));
                const u32 rd = d + ds::kReader;
                r = static_cast<s32>(module_call(ctx, "cellPamf", "cellPamfReaderInitialize", {rd, buf, 0, 1}));
                if (r < 0) {
                    free_through_thread(ctx, p, 0x80, buf);
                    break;
                }
                const u32 mux = static_cast<u32>(module_call(ctx, "cellPamf", "cellPamfReaderGetMuxRateBound", {rd}));
                w32(ctx, d + ds::kMuxRate, mux * 400);
                module_call(ctx, "cellPamf", "cellPamfReaderGetPresentationStartTime", {rd, start});
                module_call(ctx, "cellPamf", "cellPamfReaderGetPresentationEndTime", {rd, end});
                w64(ctx, d + ds::kDuration, u64{r32(ctx, end) - r32(ctx, start)} << 32 |
                    (r32(ctx, end + 4) - r32(ctx, start + 4)));
                opened = true;
                break;
            }
            free_through_thread(ctx, p, 0x80, buf);
            want = static_cast<u32>(header);
        }
    }
    if (!opened) {
        command_answer(ctx, p, 0x3F, d);
        return r;
    }
    select_streams(ctx, d);
    w8(ctx, d + ds::kState, 1);
    return lv2::CELL_OK;
}

s32 cellSailDescriptorClose(PPUContext& ctx, u32 d) { return close_descriptor(ctx, d, true); }

// DumpImage(player, buffer, size): the player's state as libsail writes it for Sony's tools, each block at the next
// 64-byte boundary: the image's version (u64 0x492000), whether libsail leaves module loading to the process (u32), the
// player's 0x3D8 bytes, its audio, video and user slots, each held descriptor (0xE0 bytes, its URI's length (u32) and
// text, its elementary stream entries), the built-in source and its state, the dmux pipeline, each slot's stream state
// (0x70 bytes from +0x10), then each audio pipeline (0xD0) with its renderer (0x50), a u64 0x100 and 0x100 bytes of the
// renderer's argument, each video pipeline (0x100) with its renderer (0x60) and the same, each AU receiver's callbacks
// (0x50) and the receiver (0x1C0). The bytes it wrote, with the padding: a block that does not fit ends the image there
// (its padding counted); padding that does not fit also prints "not enougth buffer".
u32 cellSailPlayerDumpImage(PPUContext& ctx, u32 p, u32 buf, u32 size)
{
    u8* b = gb(ctx);
    u32 cursor = buf, left = size, count = 0;
    bool full = false;
    const auto short_of_room = [&] {
        StackVar text(ctx, b, 32);
        std::memcpy(b + text.addr(), "not enougth buffer\n", 20);
        lv2_lib(ctx, "_sys_printf", {text.addr()});
        full = true;
    };
    // A block at the next 64-byte boundary (or right here): false once the buffer is full.
    const auto put = [&](const u8* from, u32 n, bool align = true) {
        if (full) return false;
        const u32 pad = align ? ((cursor + 63) & ~63u) - cursor : 0;
        if (left < pad) {
            short_of_room();
            return false;
        }
        left -= pad;
        count += pad;
        cursor += pad;
        if (left < n) {
            full = true;
            return false;
        }
        std::memmove(b + cursor, from, n);
        left -= n;
        count += n;
        cursor += n;
        return true;
    };
    const auto put_guest = [&](u32 at, u32 n) {
        if (!at) {
            log_call_site(ctx, b, "cellSailPlayerDumpImage: a NULL object in the player (libsail reads address 0)");
            PS_FATAL(kChan, "cellSailPlayerDumpImage of a player without its objects");
        }
        return put(b + at, n);
    };
    u8 word[8];
    ps_write64(word, 0, 0x492000);
    if (!put(word, 8)) return count;
    ps_write32(word, 0, loads_modules(ctx) ? 0 : 1);
    if (!put(word, 4)) return count;
    if (!put_guest(p, 0x3D8)) return count;
    const u32 na = r32(ctx, p + pl::kAudioCount), nv = r32(ctx, p + pl::kVideoCount), nu = r32(ctx, p + pl::kUserCount);
    for (u32 i = 0; i < na; i++)
        if (!put_guest(audio_slot(ctx, p, i), pl::kAudioStride)) return count;
    for (u32 i = 0; i < nv; i++)
        if (!put_guest(video_slot(ctx, p, i), pl::kVideoStride)) return count;
    for (u32 i = 0; i < nu; i++)
        if (!put_guest(user_slot(ctx, p, i), pl::kUserStride)) return count;
    for (const u32 at : {p + pl::kCurrentDescriptor, p + kNextDescriptor}) {
        const u32 d = r32(ctx, at);
        if (!d) continue;
        if (!put_guest(d, 0xE0)) return count;
        if (const u32 uri = r32(ctx, d + ds::kUri)) {
            const u32 n = static_cast<u32>(std::strlen(reinterpret_cast<const char*>(b + uri))) + 1;
            ps_write32(word, 0, n);
            if (!put(word, 4)) return count;
            if (!put(b + uri, n, false)) return count;
        }
        for (u32 i = 0; i < na; i++)
            if (!put_guest(r32(ctx, d + ds::kAudio) + i * ds::kAudioStride, ds::kAudioStride)) return count;
        for (u32 i = 0; i < nv; i++)
            if (!put_guest(r32(ctx, d + ds::kVideo) + i * ds::kVideoStride, ds::kVideoStride)) return count;
        for (u32 i = 0; i < nu; i++)
            if (!put_guest(r32(ctx, d + ds::kUser) + i * ds::kUserStride, ds::kUserStride)) return count;
    }
    if (const u32 source = r32(ctx, p + pl::kProtocolSources)) {
        if (!put_guest(source, 0x80)) return count;
        if (!put_guest(r32(ctx, source + src::kArg), 0x138)) return count;
    }
    if (!put_guest(p + pl::kDmux, 0x58)) return count;
    for (u32 i = 0; i < na; i++)
        if (!put_guest(audio_slot(ctx, p, i) + 0x10, 0x70)) return count;
    for (u32 i = 0; i < nv; i++)
        if (!put_guest(video_slot(ctx, p, i) + 0x10, 0x70)) return count;
    for (u32 i = 0; i < nu; i++)
        if (!put_guest(user_slot(ctx, p, i) + 0x10, 0x70)) return count;
    ps_write64(word, 0, 0x100);
    for (u32 i = 0; i < na; i++) {
        const u32 slot = audio_slot(ctx, p, i), renderer = r32(ctx, slot + 0x150);
        if (!put_guest(slot + 0x80, 0xD0) || !put_guest(renderer, 0x50) || !put(word, 8) ||
            !put_guest(r32(ctx, renderer + 4), 0x100))
            return count;
    }
    for (u32 i = 0; i < nv; i++) {
        const u32 slot = video_slot(ctx, p, i), renderer = r32(ctx, slot + 0x180);
        if (!put_guest(slot + 0x80, 0x100) || !put_guest(renderer, 0x60) || !put(word, 8) ||
            !put_guest(r32(ctx, renderer + 4), 0x100))
            return count;
    }
    for (u32 i = 0; i < nu; i++) {
        const u32 t = r32(ctx, user_slot(ctx, p, i) + 0x80);
        if (!put_guest(t, 0x50) || !put_guest(r32(ctx, t + 0x24), 0x1C0)) return count;
    }
    return count;
}

// OpenStream / CloseStream(player, u64 argument): the command's answer; the call's completion event carries the argument.
s32 cellSailPlayerOpenStream(PPUContext& ctx, u32 p, u64 arg)
{
    StackVar a(ctx, gb(ctx), 8);
    w64(ctx, a.addr(), arg);
    return command_answer(ctx, p, 0x4C, a.addr());
}

s32 cellSailPlayerCloseStream(PPUContext& ctx, u32 p, u64 arg)
{
    StackVar a(ctx, gb(ctx), 8);
    w64(ctx, a.addr(), arg);
    return command_answer(ctx, p, 0x5D, a.addr());
}

// SetPaused(player, paused): the paused state now.
s32 cellSailPlayerSetPaused(PPUContext& ctx, u32 p, u32 paused) { return command_answer(ctx, p, 0x44, paused); }

// SetRepeatMode(player, mode, info): the mode.
s32 cellSailPlayerSetRepeatMode(PPUContext& ctx, u32 p, u32 mode, u32 info)
{
    StackVar args(ctx, gb(ctx), 16);
    w32(ctx, args.addr(), mode);
    w32(ctx, args.addr() + 4, info);
    return command_answer(ctx, p, 0x45, args.addr());
}

// SetEsAudioMuted / SetEsVideoMuted(player, index, muted): the stream's muted bit afterwards.
s32 set_es_muted(PPUContext& ctx, u32 p, u32 index, u32 muted, bool video)
{
    if (index >= r32(ctx, p + (video ? pl::kVideoCount : pl::kAudioCount))) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
    StackVar args(ctx, gb(ctx), 16);
    w32(ctx, args.addr(), index);
    w8(ctx, args.addr() + 4, static_cast<u8>(muted));
    StackVar reply(ctx, gb(ctx), 16);
    if (const s32 r = command(ctx, p, video ? 0x47 : 0x46, args.addr(), reply.addr()); r < 0) return r;
    const u32 renderer = r32(ctx, video ? video_slot(ctx, p, index) + 0x180 : audio_slot(ctx, p, index) + 0x150);
    return static_cast<s32>(r32(ctx, renderer + 0x34) & 1);
}

s32 cellSailPlayerSetEsAudioMuted(PPUContext& ctx, u32 p, u32 index, u32 muted) { return set_es_muted(ctx, p, index, muted, false); }
s32 cellSailPlayerSetEsVideoMuted(PPUContext& ctx, u32 p, u32 index, u32 muted) { return set_es_muted(ctx, p, index, muted, true); }


s32 cellSailPlayerCreateDescriptor(PPUContext& ctx, u32 p, u32 type, u32 media_info, u32 uri, u32 out)
{
    if (!out) return error(CELL_SAIL_ERROR_INVALID_ARG);
    StackVar args(ctx, gb(ctx), 16);
    w32(ctx, args.addr(), type);
    w32(ctx, args.addr() + 4, media_info);
    w32(ctx, args.addr() + 8, uri);
    w32(ctx, args.addr() + 12, out);
    return command_answer(ctx, p, 0x40, args.addr());
}

s32 cellSailPlayerAddDescriptor(PPUContext& ctx, u32 p, u32 d)
{
    if (!d) return error(CELL_SAIL_ERROR_INVALID_ARG);
    return command_answer(ctx, p, 0x42, d);
}

s32 cellSailPlayerRemoveDescriptor(PPUContext& ctx, u32 p, u32 out) { return command_answer(ctx, p, 0x43, out); }

// Destroy is posted, not waited for.
s32 cellSailPlayerDestroyDescriptor(PPUContext& ctx, u32 p, u32 d)
{
    if (!d) return error(CELL_SAIL_ERROR_INVALID_ARG);
    if (r8(ctx, p + pl::kState) == pl::kInitialized) return error(CELL_SAIL_ERROR_INVALID_STATE);
    post_urgent(ctx, p, 0x41, 0, d, 0);
    return lv2::CELL_OK;
}

} // namespace

void register_cellSailControl(Registry& r)
{
    r.add("cellSail", {
        ::pspocket::hle::Function{{}, "cellSailPlayerBoot", recomp::nid_of("cellSailPlayerBoot"),
            &detail::thunk<&cellSailPlayerBoot>, false},
        ::pspocket::hle::Function{{}, "cellSailPlayerCreateDescriptor", recomp::nid_of("cellSailPlayerCreateDescriptor"),
            &detail::thunk<&cellSailPlayerCreateDescriptor>, false},
        ::pspocket::hle::Function{{}, "cellSailPlayerAddDescriptor", recomp::nid_of("cellSailPlayerAddDescriptor"),
            &detail::thunk<&cellSailPlayerAddDescriptor>, false},
        ::pspocket::hle::Function{{}, "cellSailPlayerRemoveDescriptor", recomp::nid_of("cellSailPlayerRemoveDescriptor"),
            &detail::thunk<&cellSailPlayerRemoveDescriptor>, false},
        ::pspocket::hle::Function{{}, "cellSailPlayerDestroyDescriptor",
            recomp::nid_of("cellSailPlayerDestroyDescriptor"), &detail::thunk<&cellSailPlayerDestroyDescriptor>, false},
        ::pspocket::hle::Function{{}, "cellSailDescriptorOpen", recomp::nid_of("cellSailDescriptorOpen"),
            &detail::thunk<&cellSailDescriptorOpen>, false},
        ::pspocket::hle::Function{{}, "cellSailDescriptorClose", recomp::nid_of("cellSailDescriptorClose"),
            &detail::thunk<&cellSailDescriptorClose>, false},
        ::pspocket::hle::Function{{}, "cellSailPlayerSetPaused", recomp::nid_of("cellSailPlayerSetPaused"),
            &detail::thunk<&cellSailPlayerSetPaused>, false},
        ::pspocket::hle::Function{{}, "cellSailPlayerDumpImage", recomp::nid_of("cellSailPlayerDumpImage"),
            &detail::thunk<&cellSailPlayerDumpImage>, false},
        ::pspocket::hle::Function{{}, "cellSailPlayerOpenStream", recomp::nid_of("cellSailPlayerOpenStream"),
            &detail::thunk<&cellSailPlayerOpenStream>, false},
        ::pspocket::hle::Function{{}, "cellSailPlayerCloseStream", recomp::nid_of("cellSailPlayerCloseStream"),
            &detail::thunk<&cellSailPlayerCloseStream>, false},
        ::pspocket::hle::Function{{}, "cellSailPlayerSetRepeatMode", recomp::nid_of("cellSailPlayerSetRepeatMode"),
            &detail::thunk<&cellSailPlayerSetRepeatMode>, false},
        ::pspocket::hle::Function{{}, "cellSailPlayerSetEsAudioMuted", recomp::nid_of("cellSailPlayerSetEsAudioMuted"),
            &detail::thunk<&cellSailPlayerSetEsAudioMuted>, false},
        ::pspocket::hle::Function{{}, "cellSailPlayerSetEsVideoMuted", recomp::nid_of("cellSailPlayerSetEsVideoMuted"),
            &detail::thunk<&cellSailPlayerSetEsVideoMuted>, false},
        ::pspocket::hle::Function{{}, "_pspocket_cellSail_control", recomp::nid_of("_pspocket_cellSail_control"),
            &detail::thunk<&_pspocket_cellSail_control>, true},
    });
}

namespace sail {

// The OpenStream steps the playback reuses (libsail's): a descriptor made ready, a stream taken down.
void prepare_descriptor(PPUContext& ctx, u32 p, u32 d, u32 rec, u64 handle) { stream_prepare(ctx, p, d, rec, handle); }
void close_stream(PPUContext& ctx, u32 p, u32 d, u32 rec, u64 handle) { stream_close(ctx, p, d, rec, handle); }
void close_descriptor_for(PPUContext& ctx, u32 d) { close_descriptor(ctx, d, false); }

} // namespace sail

} // namespace pspocket::hle
