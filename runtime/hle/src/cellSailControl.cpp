// cellSail's control thread (D-562, part 6): Boot and the player's thread, as libsail (fw 4.92) has them. Boot checks
// every stream has its renderer (or adapter, or AU receiver), keeps the game's argument and starts "_libsail-control"
// (priority and stack from the player). The thread holds the player's lock while it sets up: SPURS of its own when the
// game gave none (4 KB, "_libsail" its threads' prefix, the player's SPU count and priorities), the dmux pipeline,
// then state 2 and the events (state changed to 2; boot completed with the game's argument); it releases the lock and
// runs the message loop: take the next message (waiting 3 s at a time), dispatch by its code. Stop (0xD1, posted by
// Finalize) ends the loop; the thread takes the lock again, takes the player down (the dmux pipeline, the descriptors),
// releases it, finalizes and frees its SPURS, sets state 10 and exits. A failure in set-up sends state 9 and boot
// completed with the error.
//
// This part plays nothing: the per-stream pipelines and renderers and the other messages come in later parts. A message
// whose handler is not here yet stops the process with its code.

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

constexpr u32 kStackStat = 0x3D0; // the deepest the event handler's stack went (u64), libsail's
constexpr u32 kMessage = 0x19C;   // the message being handled (200 while waiting)
constexpr u32 kBootRunning = 0x250;

s32 lock(PPUContext& ctx, u32 p) { return sc(ctx, 102, {r32(ctx, p + pl::kLock), 0}); }
s32 unlock(PPUContext& ctx, u32 p) { return sc(ctx, 104, {r32(ctx, p + pl::kLock)}); }

u32 alloc(PPUContext& ctx, u32 p, u32 boundary, u32 size)
{
    return static_cast<u32>(call_guest(ctx, r32(ctx, p + pl::kAlloc), {r32(ctx, p + pl::kAllocArg), boundary, size}));
}

void dealloc(PPUContext& ctx, u32 p, u32 boundary, u32 ptr)
{
    call_guest(ctx, r32(ctx, p + pl::kFree), {r32(ctx, p + pl::kAllocArg), boundary, ptr});
}

u64 module_call(PPUContext& ctx, std::string_view module, std::string_view name, std::initializer_list<u64> args)
{
    const Function* f = registry().find(module, recomp::nid_of(name));
    if (!f) PS_FATAL(kChan, "{}::{}, which libsail imports, is not pspocket's", module, name);
    u64 saved[8];
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 3;
    for (const u64 a : args) ctx.gpr[i++] = a;
    f->fn(ctx, gb(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

// ------------------------------------------------------------------------------------------------- events

// The game's handler(arg, event, a, b), the event (major << 32 | minor); the depth of the control thread's stack at
// the call kept, as libsail does.
void notify(PPUContext& ctx, u32 p, u64 event, u64 a, u64 b)
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

// ------------------------------------------------------------------------------------------------- the dmux pipeline

namespace dmx {
constexpr u32 kState = 0x10, kPlayer = 0x34, kOpen = 0x48, kSize = 0x58;
}

void dmux_init(PPUContext& ctx, u32 d, u32 p)
{
    std::memset(gb(ctx) + d, 0, dmx::kSize);
    w32(ctx, d + dmx::kState, 0);
    w32(ctx, d + dmx::kPlayer, p);
}

void dmux_fini(PPUContext& ctx, u32 d)
{
    if (r32(ctx, d + dmx::kOpen) == 1) {
        log_call_site(ctx, gb(ctx), "cellSail: an open dmux at the player's end: not pspocket's yet (D-562)");
        PS_FATAL(kChan, "libsail's dmux close at the end of play is a later part of D-562");
    }
    w32(ctx, d + dmx::kState, 0xC);
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
            if (r32(ctx, p + pl::kAudioCount) || r32(ctx, p + pl::kVideoCount) || r32(ctx, p + pl::kUserCount)) {
                log_call_site(ctx, gb(ctx), "cellSail: a player with streams: libsail's pipelines are a later part");
                PS_FATAL(kChan, "libsail's stream pipelines are a later part of D-562");
            }
            dmux_init(ctx, p + pl::kDmux, p);
            w8(ctx, p + pl::kState, 2);
            notify_event(ctx, p, 3, 2, 0);
            notify_completed(ctx, p, 1, 0);
            sc(ctx, 108, {r32(ctx, p + pl::kLock + 4)});
            unlock(ctx, p);
            message_loop(ctx, p);
            lock(ctx, p);
            // The descriptors the player holds back to their open state, then freed.
            for (const u32 at : {p + pl::kCurrentDescriptor, p + 0x228u}) {
                if (const u32 d = r32(ctx, at)) {
                    const u8 st = r8(ctx, d + 0x38);
                    if (st == 2) w8(ctx, d + 0x38, 0);
                    else if (st >= 3 && st <= 5) w8(ctx, d + 0x38, 1);
                }
            }
            w32(ctx, p + pl::kDescriptorCount, 0);
            dmux_fini(ctx, p + pl::kDmux);
            if (r32(ctx, p + 0x3BC)) {
                log_call_site(ctx, gb(ctx), "cellSail: descriptors at the player's end: not pspocket's yet");
                PS_FATAL(kChan, "libsail's descriptor teardown is a later part of D-562");
            }
            w32(ctx, p + 0x3BC, 0);
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

} // namespace

void register_cellSailControl(Registry& r)
{
    r.add("cellSail", {
        ::pspocket::hle::Function{{}, "cellSailPlayerBoot", recomp::nid_of("cellSailPlayerBoot"),
            &detail::thunk<&cellSailPlayerBoot>, false},
        ::pspocket::hle::Function{{}, "_pspocket_cellSail_control", recomp::nid_of("_pspocket_cellSail_control"),
            &detail::thunk<&_pspocket_cellSail_control>, true},
    });
}

} // namespace pspocket::hle
