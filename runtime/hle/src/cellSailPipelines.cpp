// cellSail's per-stream pipelines (D-562, part 9), as libsail (fw 4.92) has them: what Boot sets up for each stream and
// the player's end takes down. An audio pipeline (0xD0 bytes at the audio slot's +0x80) holds a mutex and condition
// pair "_saiad0" (+0x78), an lwmutex "_saiad1" (+0x80) and its thread "_libsail-adec_copy" (priority 450, 4 KB of stack;
// +0x70), which takes commands (+0x98) and counts of decoded frames (+0x9C) under the pair; a video pipeline (0x100
// bytes at the video slot's +0x80) a pair "_saivd0" (+0x40), an lwmutex "_saivd1" (+0x48) and "_libsail-vdec_copy"
// (priority 550; +0x38), with commands at +0x60 (-1 idle) and counts at +0x64. Command 5 (audio) and 2 (video) end the
// threads. Renderers (the game's, or the adapters') and the user streams' AU receivers are made up at Boot and cleaned
// up at the end through their callbacks.
//
// This part plays nothing: a copy thread given anything but its end, or a pipeline holding a decoder, stops the process
// with what was asked (later parts).

#include <cstring>
#include <format>

#include "cellSail.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {

namespace sail {

// An lv2 mutex and condition at `at` and `at + 4` (priority, not recursive; `name`), libsail's pair.
s32 make_pair(PPUContext& ctx, u32 at, const char* name)
{
    u8* b = gb(ctx);
    StackVar v(ctx, b, 0x40);
    const u32 m = v.addr() + 0x18, c = v.addr();
    ps_write32(b, m, 2);
    ps_write32(b, m + 4, 0x20);
    ps_write32(b, m + 8, 0x200);
    ps_write32(b, m + 12, 0x2000);
    ps_write32(b, m + 0x18, 0);
    std::memcpy(b + m + 0x20, name, 8);
    ps_write32(b, c, 0x200);
    ps_write32(b, c + 4, 0);
    std::memcpy(b + c + 0x10, name, 8);
    s32 r = sc(ctx, 100, {at, m});
    if (r < 0) return r;
    r = sc(ctx, 105, {at + 4, r32(ctx, at), c});
    if (r < 0) sc(ctx, 101, {r32(ctx, at)});
    return r;
}

void destroy_pair(PPUContext& ctx, u32 at)
{
    sc(ctx, 106, {r32(ctx, at + 4)});
    sc(ctx, 101, {r32(ctx, at)});
}

// sys_lwmutex_create(at, {priority, not recursive, name}), libsail's.
s32 make_lwmutex(PPUContext& ctx, u32 at, const char* name)
{
    u8* b = gb(ctx);
    StackVar v(ctx, b, 16);
    ps_write32(b, v.addr(), 2);
    ps_write32(b, v.addr() + 4, 0x20);
    std::memcpy(b + v.addr() + 8, name, 8);
    return static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_create", {at, v.addr()}));
}

s32 destroy_lwmutex(PPUContext& ctx, u32 at) { return static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_destroy", {at})); }

} // namespace sail

namespace {

using namespace sail;

namespace ap {
constexpr u32 kAdec = 0x40, kAdecModule = 0x44, kPostModule = 0x48, kAlive = 0x4C, kThread = 0x70, kPair = 0x78,
              kLwmutex = 0x80, kCommand = 0x98, kCount = 0x9C, kPlayer = 0xA4, kBuffer = 0xB4, kPost = 0xBC, kSize = 0xD0;
constexpr u32 kStop = 5;
} // namespace ap

namespace vp {
constexpr u32 kVdec = 8, kAlive = 0xC, kBuffer = 0x30, kThread = 0x38, kPair = 0x40, kLwmutex = 0x48, kCommand = 0x60,
              kCount = 0x64, kPlayer = 0x70, kStream = 0x7C, kVdecModule = 0x80, kSize = 0x100;
constexpr u32 kIdle = 0xFFFFFFFF, kStop = 2;
} // namespace vp

u32 internal(std::string_view name) { return HleProcess::current()->link_function("cellSail", recomp::nid_of(name), name); }

void later(PPUContext& ctx, std::string_view what)
{
    log_call_site(ctx, gb(ctx), std::format("cellSail: {}: not pspocket's yet", what));
    PS_FATAL(kChan, "libsail's {} is a later part of D-562", what);
}

s32 unload_module(PPUContext& ctx, u32 id)
{
    // The pipelines' modules are loaded once they play (later parts); unloading one now would be such a part's.
    later(ctx, std::format("unloading module {:#x} at a pipeline's end", id));
    return 0;
}

// ------------------------------------------------------------------------------------------------- the copy threads

// "_libsail-adec_copy": under the pair, takes the command and the frames counted since (both cleared, the condition
// signalled), waiting while there are neither; command 5 ends it.
void _pspocket_cellSail_adecCopy(PPUContext& ctx, u64 pipeline)
{
    const u32 a = static_cast<u32>(pipeline);
    const u32 mutex = r32(ctx, a + ap::kPair), cond = r32(ctx, a + ap::kPair + 4);
    for (;;) {
        sc(ctx, 102, {mutex, 0});
        u32 command = r32(ctx, a + ap::kCommand), count = r32(ctx, a + ap::kCount);
        w32(ctx, a + ap::kCommand, 0);
        w32(ctx, a + ap::kCount, 0);
        sc(ctx, 108, {cond});
        while (command == 0 && count == 0) {
            sc(ctx, 107, {cond, 0});
            command = r32(ctx, a + ap::kCommand);
            count = r32(ctx, a + ap::kCount);
            w32(ctx, a + ap::kCommand, 0);
            w32(ctx, a + ap::kCount, 0);
            sc(ctx, 108, {cond});
        }
        sc(ctx, 104, {mutex});
        if (command == ap::kStop) lv2_lib(ctx, "sys_ppu_thread_exit", {0});
        later(ctx, std::format("the audio copy thread's command {} with {} frames", command, count));
    }
}

// "_libsail-vdec_copy": under the pair, takes the command (leaving it idle, -1, the condition signalled), waiting while
// it is idle; command 2 ends it.
void _pspocket_cellSail_vdecCopy(PPUContext& ctx, u64 pipeline)
{
    const u32 v = static_cast<u32>(pipeline);
    const u32 mutex = r32(ctx, v + vp::kPair), cond = r32(ctx, v + vp::kPair + 4);
    for (;;) {
        sc(ctx, 102, {mutex, 0});
        u32 command = r32(ctx, v + vp::kCommand);
        w32(ctx, v + vp::kCommand, vp::kIdle);
        sc(ctx, 108, {cond});
        while (command == vp::kIdle) {
            sc(ctx, 107, {cond, 0});
            command = r32(ctx, v + vp::kCommand);
            w32(ctx, v + vp::kCommand, vp::kIdle);
            sc(ctx, 108, {cond});
        }
        const bool frame = command == 1 && static_cast<s32>(r32(ctx, v + vp::kCount)) > 0;
        if (frame) w32(ctx, v + vp::kCount, r32(ctx, v + vp::kCount) - 1);
        sc(ctx, 104, {mutex});
        if (frame) later(ctx, "the video copy thread's frame");
        if (command == vp::kStop) lv2_lib(ctx, "sys_ppu_thread_exit", {0});
        if (command == 0) later(ctx, "the video copy thread's reset");
        // Others (and 1 without frames): back to waiting.
    }
}

s32 create_thread(PPUContext& ctx, u32 id_at, std::string_view entry, u32 arg, u32 prio, const char* name)
{
    StackVar text(ctx, gb(ctx), 32);
    std::memcpy(gb(ctx) + text.addr(), name, std::strlen(name) + 1);
    return static_cast<s32>(lv2_lib(ctx, "sys_ppu_thread_create",
        {id_at, internal(entry), arg, prio, 0x1000, 1, text.addr()}));
}

} // namespace

namespace sail {

// ------------------------------------------------------------------------------------------------- audio

s32 audio_pipeline_init(PPUContext& ctx, u32 a, u32 p)
{
    std::memset(gb(ctx) + a, 0, ap::kSize);
    w32(ctx, a + ap::kPlayer, p);
    w32(ctx, a + ap::kAlive, 1);
    s32 r = make_pair(ctx, a + ap::kPair, "_saiad0");
    if (r < 0) return r;
    r = make_lwmutex(ctx, a + ap::kLwmutex, "_saiad1");
    if (r >= 0) {
        w32(ctx, a + ap::kCommand, 0);
        r = create_thread(ctx, a + ap::kThread, "_pspocket_cellSail_adecCopy", a, 450, "_libsail-adec_copy");
        if (r >= 0) return lv2::CELL_OK;
        destroy_lwmutex(ctx, a + ap::kLwmutex);
    }
    destroy_pair(ctx, a + ap::kPair);
    return r;
}

// The audio thread told `command` once it has taken the last one.
void audio_command(PPUContext& ctx, u32 a, u32 command)
{
    const u32 mutex = r32(ctx, a + ap::kPair), cond = r32(ctx, a + ap::kPair + 4);
    sc(ctx, 102, {mutex, 0});
    while (r32(ctx, a + ap::kCommand) != 0) sc(ctx, 107, {cond, 0});
    w32(ctx, a + ap::kCommand, command);
    sc(ctx, 108, {cond});
    sc(ctx, 104, {mutex});
}

void audio_pipeline_stop(PPUContext& ctx, u32 a)
{
    if (!r64(ctx, a + ap::kThread)) return;
    audio_command(ctx, a, ap::kStop);
    StackVar status(ctx, gb(ctx), 8);
    sc(ctx, 44, {r64(ctx, a + ap::kThread), status.addr()});
    w64(ctx, a + ap::kThread, 0);
}

void audio_pipeline_fini(PPUContext& ctx, u32 a)
{
    audio_pipeline_stop(ctx, a);
    // The decoder (cellAdec) and its buffer, the post-processor (cellApostSrcMini): only once played.
    if (r32(ctx, a + ap::kAdec) || r32(ctx, a + ap::kPost)) later(ctx, "closing an audio pipeline's decoder");
    const u32 p = r32(ctx, a + ap::kPlayer);
    if (const u32 buf = r32(ctx, a + ap::kBuffer)) {
        call_guest(ctx, r32(ctx, p + pl::kFree), {r32(ctx, p + pl::kAllocArg), 0x80, buf});
    }
    if (!r32(ctx, a + ap::kAdecModule) || unload_module(ctx, r32(ctx, a + ap::kAdecModule) & 0xFFFF) >= 0) {
        w32(ctx, a + ap::kAdecModule, 0);
        w32(ctx, a + ap::kAdec, 0);
        w32(ctx, a + ap::kBuffer, 0);
    }
    if (const u32 m = r32(ctx, a + ap::kPostModule)) {
        unload_module(ctx, m & 0xFFFF);
        w32(ctx, a + ap::kPostModule, 0);
    }
    destroy_lwmutex(ctx, a + ap::kLwmutex);
    destroy_pair(ctx, a + ap::kPair);
    w32(ctx, a + ap::kAlive, 0);
}

// ------------------------------------------------------------------------------------------------- video

s32 video_pipeline_init(PPUContext& ctx, u32 v, u32 p)
{
    std::memset(gb(ctx) + v, 0, vp::kSize);
    w32(ctx, v + vp::kPlayer, p);
    w32(ctx, v + vp::kAlive, 1);
    s32 r = make_pair(ctx, v + vp::kPair, "_saivd0");
    if (r < 0) return r;
    r = make_lwmutex(ctx, v + vp::kLwmutex, "_saivd1");
    if (r >= 0) {
        w32(ctx, v + vp::kCommand, vp::kIdle);
        r = create_thread(ctx, v + vp::kThread, "_pspocket_cellSail_vdecCopy", v, 550, "_libsail-vdec_copy");
        if (r >= 0) return lv2::CELL_OK;
        destroy_lwmutex(ctx, v + vp::kLwmutex);
    }
    destroy_pair(ctx, v + vp::kPair);
    return r;
}

void video_pipeline_stop(PPUContext& ctx, u32 v)
{
    if (!r64(ctx, v + vp::kThread)) return;
    const u32 mutex = r32(ctx, v + vp::kPair), cond = r32(ctx, v + vp::kPair + 4);
    sc(ctx, 102, {mutex, 0});
    while (r32(ctx, v + vp::kCommand) != vp::kIdle) sc(ctx, 107, {cond, 0});
    w32(ctx, v + vp::kCommand, vp::kStop);
    sc(ctx, 108, {cond});
    sc(ctx, 104, {mutex});
    StackVar status(ctx, gb(ctx), 8);
    sc(ctx, 44, {r64(ctx, v + vp::kThread), status.addr()});
    w64(ctx, v + vp::kThread, 0);
}

void video_pipeline_fini(PPUContext& ctx, u32 v)
{
    video_pipeline_stop(ctx, v);
    // The decoder (cellVdec) and the post-processor (cellVpost) of an open stream: only once played.
    if (r32(ctx, v + vp::kVdec) || r32(ctx, v + vp::kStream)) later(ctx, "closing a video pipeline's decoder");
    const u32 p = r32(ctx, v + vp::kPlayer);
    if (const u32 buf = r32(ctx, v + vp::kBuffer)) {
        call_guest(ctx, r32(ctx, p + pl::kFree), {r32(ctx, p + pl::kAllocArg), 0x80, buf});
    }
    if (!r32(ctx, v + vp::kVdecModule) || unload_module(ctx, r32(ctx, v + vp::kVdecModule) & 0xFFFF) >= 0) {
        w32(ctx, v + vp::kVdecModule, 0);
        w32(ctx, v + vp::kVdec, 0);
        w32(ctx, v + vp::kBuffer, 0);
    }
    destroy_lwmutex(ctx, v + vp::kLwmutex);
    destroy_pair(ctx, v + vp::kPair);
    w32(ctx, v + vp::kAlive, 0);
}

// ------------------------------------------------------------------------------------------------- renderers, receivers

// A renderer (the game's, or an adapter): +0 its callback table, +4 its argument, +0x1C its state (0 made up, 9 cleaned
// up). Makeup when it has one.
s32 renderer_makeup(PPUContext& ctx, u32 r)
{
    const u32 funcs = r32(ctx, r);
    if (const u32 makeup = r32(ctx, funcs)) {
        const s32 e = static_cast<s32>(call_guest(ctx, makeup, {r32(ctx, r + 4)}));
        if (e < 0) return e;
    }
    w32(ctx, r + 0x1C, 0);
    return lv2::CELL_OK;
}

// Closed first unless made up only or cleaned up already, then cleanup when it has one.
s32 renderer_cleanup(PPUContext& ctx, u32 r)
{
    const u32 funcs = r32(ctx, r), st = r32(ctx, r + 0x1C);
    if (st != 0 && st != 9) call_guest(ctx, r32(ctx, funcs + 12), {r32(ctx, r + 4)});
    w32(ctx, r + 0x1C, 9);
    const u32 cleanup = r32(ctx, funcs + 4);
    return cleanup ? static_cast<s32>(call_guest(ctx, cleanup, {r32(ctx, r + 4)})) : 0;
}

// A user stream's AU receiver from its callbacks (+0x20 of the receiver: 9 descriptors, then +0x24 its argument):
// the player, its clock and the stream's index kept, makeup, state 0 (+0x38).
s32 receiver_makeup(PPUContext& ctx, u32 t, u32 p, u32 clock, u32 index)
{
    w32(ctx, t + 0x30, p);
    w32(ctx, t + 0x2C, clock);
    w32(ctx, t + 0x28, index);
    if (const u32 makeup = r32(ctx, t)) {
        const s32 e = static_cast<s32>(call_guest(ctx, makeup, {r32(ctx, t + 0x24)}));
        if (e < 0) return e;
    }
    w32(ctx, t + 0x38, 0);
    return lv2::CELL_OK;
}

s32 receiver_cleanup(PPUContext& ctx, u32 t)
{
    const u32 st = r32(ctx, t + 0x38);
    if (st != 0 && st != 8) call_guest(ctx, r32(ctx, t + 0xC), {r32(ctx, t + 0x24)});
    w32(ctx, t + 0x38, 8);
    const u32 cleanup = r32(ctx, t + 4);
    return cleanup ? static_cast<s32>(call_guest(ctx, cleanup, {r32(ctx, t + 0x24)})) : 0;
}

} // namespace sail

void register_cellSailPipelines(Registry& r)
{
    r.add("cellSail", {
        ::pspocket::hle::Function{{}, "_pspocket_cellSail_adecCopy", recomp::nid_of("_pspocket_cellSail_adecCopy"),
            &detail::thunk<&_pspocket_cellSail_adecCopy>, true},
        ::pspocket::hle::Function{{}, "_pspocket_cellSail_vdecCopy", recomp::nid_of("_pspocket_cellSail_vdecCopy"),
            &detail::thunk<&_pspocket_cellSail_vdecCopy>, true},
    });
}

} // namespace pspocket::hle
