// cellSail's player (D-562, part 2): its life before it plays, as libsail (fw 4.92) has it. Initialize lays the player
// out in the game's 0x400 bytes and one block from the game's allocator: the audio, video and user stream slots (0x180,
// 0x238 and 0x88 bytes each), the built-in "x-cell-fs" source (0x138 bytes) and the message ring; it makes the queue's
// and the player's lv2 mutex and condition pairs ("_saiqu0", "_saict0"), the AV-sync clock (200 bytes at +8, its
// lwmutex "_saiav0", latencies by the player's mode), registers the built-in source and sets the defaults (control
// thread priority 900 and 0x3000 bytes of stack; SPURS with 4 SPUs at priority 100, PPU priority 300). Finalize undoes
// it, first stopping the control thread when it was booted. The event mask, the event handler, the registered sources
// (four at most, by protocol name), the per-stream adapters and renderers, and the getters under the player's lock.

#include <algorithm>
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

// ------------------------------------------------------------------------------------------------- helpers

// The game's allocator (CellSailMemAllocator copied to +0x23C): alloc(arg, boundary, size), free(arg, boundary, ptr).
u32 alloc(PPUContext& ctx, u32 p, u32 boundary, u32 size)
{
    return static_cast<u32>(call_guest(ctx, r32(ctx, p + pl::kAlloc), {r32(ctx, p + pl::kAllocArg), boundary, size}));
}

void dealloc(PPUContext& ctx, u32 p, u32 boundary, u32 ptr)
{
    call_guest(ctx, r32(ctx, p + pl::kFree), {r32(ctx, p + pl::kAllocArg), boundary, ptr});
}

// An lv2 mutex and condition at `at` and `at + 4` (priority, not recursive; `name`), libsail's pair.
s32 pair_create(PPUContext& ctx, u32 at, const char* name)
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

s32 pair_destroy(PPUContext& ctx, u32 at)
{
    sc(ctx, 106, {r32(ctx, at + 4)});
    return sc(ctx, 101, {r32(ctx, at)});
}

// sys_lwmutex_create(at, {priority, not recursive, name}), libsail's.
s32 lwmutex_create(PPUContext& ctx, u32 at, const char* name)
{
    u8* b = gb(ctx);
    StackVar v(ctx, b, 16);
    ps_write32(b, v.addr(), 2);
    ps_write32(b, v.addr() + 4, 0x20);
    std::memcpy(b + v.addr() + 8, name, 8);
    return static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_create", {at, v.addr()}));
}

s32 lock(PPUContext& ctx, u32 p) { return sc(ctx, 102, {r32(ctx, p + pl::kLock), 0}); }
s32 unlock(PPUContext& ctx, u32 p) { return sc(ctx, 104, {r32(ctx, p + pl::kLock)}); }

// What libsail's module start reads once: the process's SDK version (sys_process_get_sdk_version) and the default it
// gives a video stream's +0x188 (2; module start arguments may change it, which pspocket's modules do not have).
struct SailGlobals {
    bool read = false;
    u32 sdk = 0;
};

u32 sdk_version(PPUContext& ctx)
{
    SailGlobals& g = HleProcess::current()->state<SailGlobals>();
    if (!g.read) {
        g.read = true;
        g.sdk = thread_of(ctx).kernel.config().sdk_version;
    }
    return g.sdk;
}

// ------------------------------------------------------------------------------------------------- AV sync

// The clock (200 bytes at the player's +8): +0 90 (ticks per millisecond), +0x18/+0x20 the first latency in
// microseconds and in ticks, +0x38/+0x40 the second, +0x48/+0x4C -1, +0x80 1 when there is no first latency, +0xB0
// its lwmutex "_saiav0"; its reset (the play state) after.
namespace av {
constexpr u32 kTicks = 0, kLatency = 0x18, kLatencyTicks = 0x20, kLatency2 = 0x38, kLatency2Ticks = 0x40,
              kNoLatency = 0x80, kMutex = 0xB0, kSize = 200;
}

void avsync_reset_play(PPUContext& ctx, u32 a)
{
    ps_write16(gb(ctx), a + 0x8A, 0);
    w32(ctx, a + 0xA0, 0xFFFFFFFF);
    w64(ctx, a + 0x78, ~u64{0});
    w32(ctx, a + 0xA4, 0);
    w32(ctx, a + 0xA8, 0);
    w32(ctx, a + 0x94, 0xFFFFFFFF);
    w32(ctx, a + 0x98, 0xFFFFFFFF);
    w32(ctx, a + 0x9C, 0xFFFFFFFF);
    w64(ctx, a + 0x70, ~u64{0});
    ps_write16(gb(ctx), a + 0x8C, 0);
}

void avsync_reset(PPUContext& ctx, u32 a)
{
    ps_write16(gb(ctx), a + 0x8E, 0);
    ps_write16(gb(ctx), a + 0x88, 0);
    avsync_reset_play(ctx, a);
}

s32 avsync_init(PPUContext& ctx, u32 a, u32 latency, u32 latency2)
{
    std::memset(gb(ctx) + a, 0, av::kSize);
    const s32 r = lwmutex_create(ctx, a + av::kMutex, "_saiav0");
    if (r < 0) return r;
    w32(ctx, a + 8, 0);
    w64(ctx, a + av::kTicks, 90);
    w32(ctx, a + 0x28, 0);
    const u64 l1 = static_cast<u32>(latency * 1000u) / 1000u;
    w64(ctx, a + av::kLatency, l1);
    w32(ctx, a + av::kNoLatency, latency == 0 ? 1 : 0);
    w32(ctx, a + 0x4C, 0xFFFFFFFF);
    w32(ctx, a + 0x48, 0xFFFFFFFF);
    w64(ctx, a + av::kLatencyTicks, static_cast<u64>(static_cast<s64>(l1 * 90) / 1000));
    const u64 l2 = static_cast<u32>(latency2 * 1000u) / 1000u;
    w64(ctx, a + av::kLatency2, l2);
    w64(ctx, a + av::kLatency2Ticks, static_cast<u64>(static_cast<s64>(l2 * 90) / 1000));
    avsync_reset(ctx, a);
    return lv2::CELL_OK;
}

s32 avsync_fini(PPUContext& ctx, u32 a)
{
    return static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_destroy", {a + av::kMutex}));
}

// ------------------------------------------------------------------------------------------------- the built-in source

// libsail's file source ("x-cell-fs", 0x138 bytes): +0 its state, +0x40 the CellSailSource it registers, +0x100 its
// file calls (14 words: -1, then the cellFs wrappers' descriptors), the source callbacks its CellSailSource holds.
// The callbacks are reached only once the player plays (part 3): until then they fail loudly.
namespace fs {
constexpr u32 kSource = 0x40, kCalls = 0x100, kSize = 0x138;
}

// The internal functions libsail hands out by descriptor: the file source's callbacks and its cellFs wrappers.
u32 internal(std::string_view name) { return HleProcess::current()->link_function("cellSail", recomp::nid_of(name), name); }

void not_yet(PPUContext& ctx, std::string_view name)
{
    log_call_site(ctx, gb(ctx), std::format("cellSail: {} (libsail's built-in file source) is not pspocket's yet", name));
    PS_FATAL(kChan, "{}: libsail's built-in file source is not pspocket's yet (D-562 part 3)", name);
}

#define SAIL_FS_STUB(n)                                                                                                \
    s32 _pspocket_cellSail_##n(PPUContext& ctx)                                                                        \
    {                                                                                                                  \
        not_yet(ctx, #n);                                                                                              \
        return error(CELL_SAIL_ERROR_FATAL);                                                                           \
    }
SAIL_FS_STUB(fsOpen)
SAIL_FS_STUB(fsClose)
SAIL_FS_STUB(fsRead)
SAIL_FS_STUB(fsLseek)
SAIL_FS_STUB(fsFstat)
SAIL_FS_STUB(fsCancel)
SAIL_FS_STUB(srcMakeup)
SAIL_FS_STUB(srcCleanup)
SAIL_FS_STUB(srcOpen)
SAIL_FS_STUB(srcClose)
SAIL_FS_STUB(srcStart)
SAIL_FS_STUB(srcStop)
SAIL_FS_STUB(srcCancel)
SAIL_FS_STUB(srcCheckout)
SAIL_FS_STUB(srcCheckin)
SAIL_FS_STUB(srcClear)
SAIL_FS_STUB(srcRead)
SAIL_FS_STUB(srcReadSync)
SAIL_FS_STUB(srcGetCapabilities)
SAIL_FS_STUB(srcInquireCapability)
#undef SAIL_FS_STUB

s32 builtin_source_init(PPUContext& ctx, u32 s)
{
    u8* b = gb(ctx);
    std::memset(b + s, 0, fs::kSize);
    // The file calls: -1, open, 0, close, read, lseek, fstat, cancel, then nothing.
    const u32 calls[14] = {0xFFFFFFFF, internal("_pspocket_cellSail_fsOpen"), 0, internal("_pspocket_cellSail_fsClose"),
        internal("_pspocket_cellSail_fsRead"), internal("_pspocket_cellSail_fsLseek"),
        internal("_pspocket_cellSail_fsFstat"), internal("_pspocket_cellSail_fsCancel"), 0, 0, 0, 0, 0, 0};
    for (u32 i = 0; i < 14; i++) ps_write32(b, s + fs::kCalls + 4 * i, calls[i]);
    // The source callbacks (CellSailSourceFuncs), on the stack as libsail's static table.
    static constexpr std::string_view kFuncs[14] = {"_pspocket_cellSail_srcMakeup", "_pspocket_cellSail_srcCleanup",
        "_pspocket_cellSail_srcOpen", "_pspocket_cellSail_srcClose", "_pspocket_cellSail_srcStart",
        "_pspocket_cellSail_srcStop", "_pspocket_cellSail_srcCancel", "_pspocket_cellSail_srcCheckout",
        "_pspocket_cellSail_srcCheckin", "_pspocket_cellSail_srcClear", "_pspocket_cellSail_srcRead",
        "_pspocket_cellSail_srcReadSync", "_pspocket_cellSail_srcGetCapabilities",
        "_pspocket_cellSail_srcInquireCapability"};
    StackVar funcs(ctx, b, 0x38);
    for (u32 i = 0; i < 14; i++) ps_write32(b, funcs.addr() + 4 * i, internal(kFuncs[i]));
    const Function* init = registry().find("cellSail", recomp::nid_of("cellSailSourceInitialize"));
    u64 saved[3] = {ctx.gpr[3], ctx.gpr[4], ctx.gpr[5]};
    ctx.gpr[3] = s + fs::kSource;
    ctx.gpr[4] = funcs.addr();
    ctx.gpr[5] = s;
    init->fn(ctx, b);
    const s32 r = static_cast<s32>(ctx.gpr[3]);
    ctx.gpr[3] = saved[0];
    ctx.gpr[4] = saved[1];
    ctx.gpr[5] = saved[2];
    return r;
}

void builtin_source_fini(PPUContext& ctx, u32 s) { w32(ctx, s + fs::kSource + 0x48, 0); }

// ------------------------------------------------------------------------------------------------- sources

s32 cellSailPlayerRegisterSource(PPUContext& ctx, u32 p, u32 protocol, u32 source)
{
    if (r8(ctx, p + pl::kState) != pl::kInitialized) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (!protocol || !source) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const u32 n = r32(ctx, p + pl::kProtocolCount);
    if (n >= 4) return error(CELL_SAIL_ERROR_FULLED);
    const GuestString name = read_cstr(thread_of(ctx), protocol, 0x100);
    u32 i = 0;
    for (; i < r32(ctx, p + pl::kProtocolCount); i++) {
        const GuestString have = read_cstr(thread_of(ctx), p + pl::kProtocols + 0x20 * i, 0x100);
        if (have.text == name.text) return error(CELL_SAIL_ERROR_INVALID_ARG);
    }
    lv2_lib(ctx, "_sys_strncpy", {p + pl::kProtocols + 0x20 * i, protocol, 0x20});
    w32(ctx, p + pl::kProtocolCount, r32(ctx, p + pl::kProtocolCount) + 1);
    w32(ctx, p + pl::kProtocolSources + 4 * i, source);
    w8(ctx, p + pl::kProtocols + 0x20 * i + 0x1F, 0);
    return lv2::CELL_OK;
}

s32 cellSailPlayerUnregisterSource(PPUContext& ctx, u32 p, u32 protocol, u32 out)
{
    if (r8(ctx, p + pl::kState) != pl::kInitialized) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (!protocol) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const GuestString name = read_cstr(thread_of(ctx), protocol, 0x100);
    u32 n = r32(ctx, p + pl::kProtocolCount), i = 0;
    if (n) {
        for (; i < n; i++) {
            if (read_cstr(thread_of(ctx), p + pl::kProtocols + 0x20 * i, 0x100).text == name.text) break;
        }
        // libsail's check is `count < index`: a match (index below the count) or none (index equal to it) both fall
        // through to NOT_FOUND, so no source is ever unregistered.
        if (n < i) {
            if (out) w32(ctx, out, 0);
            return lv2::CELL_OK;
        }
    }
    return error(CELL_SAIL_ERROR_NOT_FOUND);
}

u32 cellSailPlayerGetRegisteredProtocols(PPUContext& ctx, u32 p, u32 names, u32 count)
{
    const u32 n = r32(ctx, p + pl::kProtocolCount);
    const u32 take = std::min(n, r32(ctx, count));
    for (u32 i = 0; i < take; i++)
        lv2_lib(ctx, "_sys_strncpy", {r32(ctx, names + 4 * i), p + pl::kProtocols + 0x20 * i, 0x20});
    return n;
}

// ------------------------------------------------------------------------------------------------- initialize

// libsail's initialize: `attr` {mode < 8, audio streams, video streams, user streams, messages (0: 50 per stream and
// 50)}; `spurs` the game's SPURS instance (Initialize2) or 0.
s32 initialize(PPUContext& ctx, u32 p, u32 allocator, u32 handler, u32 handler_arg, u32 attr, u32 spurs_at)
{
    u8* b = gb(ctx);
    std::memset(b + p, 0, pl::kSize);
    w32(ctx, p + pl::kVersion, 0x492000);
    const u32 mode = r32(ctx, attr), audio = r32(ctx, attr + 4), video = r32(ctx, attr + 8), user = r32(ctx, attr + 12);
    u32 msgs = r32(ctx, attr + 16);
    s32 r = error(CELL_SAIL_ERROR_INVALID_ARG);
    const bool ok = mode < 8 && handler && (sdk_version(ctx) < 0x400000 || (audio < 16 && video < 4)) && allocator;
    if (!ok) {
        w8(ctx, p + pl::kState, pl::kFinalized);
        return r;
    }
    std::memmove(b + p + pl::kAlloc, b + allocator, 12);
    if (spurs_at) w32(ctx, p + pl::kSpurs, r32(ctx, spurs_at));
    w32(ctx, p + pl::kHandler, handler);
    w32(ctx, p + pl::kHandlerArg, handler_arg);
    w64(ctx, p + pl::kEventMask, ~u64{0});
    w32(ctx, p + pl::kAudioCount, audio);
    w32(ctx, p + pl::kVideoCount, video);
    w32(ctx, p + pl::kUserCount, user);
    if (!msgs) msgs = (user + audio + video) * 50 + 50;
    const u32 size = msgs * 16 + user * 0x400 + audio * 0x240 + video * 0x300 + 0x428;
    r = error(CELL_SAIL_ERROR_MEMORY);
    const u32 mem = alloc(ctx, p, 8, size);
    bool made_queue = false, made_lock = false, made_sync = false, made_source = false;
    if (mem) {
        std::memset(b + mem, 0, size);
        w32(ctx, p + pl::kQueue + q::kCapacity, msgs);
        const u32 vbase = mem + audio * pl::kAudioStride, ubase = vbase + video * pl::kVideoStride,
                  src = ubase + user * pl::kUserStride;
        w32(ctx, p + pl::kVideo, vbase);
        w32(ctx, p + pl::kAudio, mem);
        w32(ctx, p + pl::kUser, ubase);
        w32(ctx, p + pl::kSource, src);
        w32(ctx, p + pl::kQueue + q::kRing, src + fs::kSize);
        r = pair_create(ctx, p + pl::kQueue + q::kMutex, "_saiqu0");
        if (r >= 0) {
            made_queue = true;
            r = pair_create(ctx, p + pl::kLock, "_saict0");
        }
        if (r >= 0) {
            made_lock = true;
            // The clock's latencies (microseconds) by mode: 0 and 2 one frame at 59.94 Hz, 3 two, 4 20 ms, 5 40 ms, 1
            // and 6 none; 7 neither.
            u32 l1 = 5333, l2 = 0;
            switch (mode) {
            case 0: case 2: l2 = 16683; break;
            case 3: l2 = 33367; break;
            case 4: l2 = 20000; break;
            case 5: l2 = 40000; break;
            case 7: l1 = 0; break;
            default: break;
            }
            r = avsync_init(ctx, p + pl::kSync, l1 & 5333, l2 & 0xDF7F);
        }
        if (r >= 0) {
            made_sync = true;
            r = builtin_source_init(ctx, src);
        }
        if (r >= 0) {
            made_source = true;
            StackVar name(ctx, b, 16);
            std::memcpy(b + name.addr(), "x-cell-fs", 10);
            r = cellSailPlayerRegisterSource(ctx, p, name.addr(), src + fs::kSource);
        }
        if (r >= 0) {
            const u32 pa = p + pl::kParams;
            w32(ctx, pa + 8, 700);
            w32(ctx, pa + 28, 1);
            w32(ctx, pa + 12, 0x1000);
            w64(ctx, pa + 20, 0x0101010101010101ull);
            for (u32 i = 0; i < audio; i++) {
                const u32 a = mem + i * pl::kAudioStride;
                w8(ctx, a + 0x154, static_cast<u8>(i));
                if (mode == 1) w16(ctx, a + 0x156, 1);
                w32(ctx, a + 0x178, 1);
                w32(ctx, a + 0x164, 450);
                w32(ctx, a + 0x168, 0x1000);
                w64(ctx, a + 0x170, 0x0101010101010101ull);
            }
            for (u32 i = 0; i < video; i++) {
                const u32 v = vbase + i * pl::kVideoStride;
                w8(ctx, v + 0x184, static_cast<u8>(i));
                if (mode == 1) w16(ctx, v + 0x186, 1);
                w64(ctx, v + 0x210, 0x0101010101010101ull);
                w32(ctx, v + 0x1FC, 0x4000);
                w32(ctx, v + 0x1F8, 0x226);
                w8(ctx, v + 0x18C, 4);
                w8(ctx, v + 0x188, 2); // libsail's module start default
                w8(ctx, v + 0x189, 4);
                w8(ctx, v + 0x18A, 6);
                w8(ctx, v + 0x18B, 1);
            }
            w32(ctx, p + pl::kThreadPrio, 900);
            w8(ctx, p + pl::kSpursExit, 1);
            w32(ctx, p + pl::kThreadStack, 0x3000);
            w32(ctx, p + pl::kSpursSpus, 4);
            w32(ctx, p + pl::kSpursSpuPrio, 100);
            w32(ctx, p + pl::kSpursPpuPrio, 300);
            return lv2::CELL_OK;
        }
        if (made_source) builtin_source_fini(ctx, src);
        if (made_sync) avsync_fini(ctx, p + pl::kSync);
        if (made_lock) pair_destroy(ctx, p + pl::kLock);
        if (made_queue && r32(ctx, p + pl::kQueue + q::kCount) != 0xFFFFFFFF)
            pair_destroy(ctx, p + pl::kQueue + q::kMutex);
        if (r32(ctx, p + pl::kAudio)) dealloc(ctx, p, 8, r32(ctx, p + pl::kAudio));
    }
    w8(ctx, p + pl::kState, pl::kFinalized);
    return r;
}

// Initialize(player, mode, allocator, handler, handler arg, audio streams, video streams, user streams, messages): the
// first form, its attribute built from the arguments.
s32 cellSailPlayerInitialize(PPUContext& ctx, u32 p, u32 mode, u32 allocator, u32 handler, u32 handler_arg, u32 audio,
    u32 video, u32 user, u32 msgs)
{
    if (!p || !allocator) return error(CELL_SAIL_ERROR_INVALID_ARG);
    StackVar attr(ctx, gb(ctx), 0x20);
    w32(ctx, attr.addr(), mode);
    w32(ctx, attr.addr() + 4, audio);
    w32(ctx, attr.addr() + 8, video);
    w32(ctx, attr.addr() + 12, user);
    w32(ctx, attr.addr() + 16, msgs);
    return initialize(ctx, p, allocator, handler, handler_arg, attr.addr(), 0);
}

// Initialize2(player, allocator, handler, handler arg, attr, resource {spurs, 0, 0, 0}).
s32 cellSailPlayerInitialize2(PPUContext& ctx, u32 p, u32 allocator, u32 handler, u32 handler_arg, u32 attr,
    u32 resource)
{
    if (!p || !attr || r32(ctx, attr + 0x14) || r32(ctx, attr + 0x18) || !resource || r32(ctx, resource + 4) ||
        r32(ctx, resource + 8) || r32(ctx, resource + 12))
        return error(CELL_SAIL_ERROR_INVALID_ARG);
    return initialize(ctx, p, allocator, handler, handler_arg, attr, resource);
}

s32 cellSailPlayerFinalize(PPUContext& ctx, u32 p)
{
    const u8 state = r8(ctx, p + pl::kState);
    if (state == pl::kFinalized) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (state != pl::kBootFailed && state != pl::kInitialized) {
        post(ctx, p, 0xD1, p, 0);
        StackVar status(ctx, gb(ctx), 8);
        sc(ctx, 44, {r64(ctx, p + pl::kThread), status.addr()}); // sys_ppu_thread_join
    }
    builtin_source_fini(ctx, r32(ctx, p + pl::kSource));
    for (u32 n = 0; n < 10; n++) {
        if (static_cast<u32>(avsync_fini(ctx, p + pl::kSync)) != 0x8001000A) break; // EBUSY: once more in 1 ms
        sc(ctx, 141, {1000});
    }
    pair_destroy(ctx, p + pl::kLock);
    if (r32(ctx, p + pl::kQueue + q::kCount) != 0xFFFFFFFF) pair_destroy(ctx, p + pl::kQueue + q::kMutex);
    if (const u32 mem = r32(ctx, p + pl::kAudio)) {
        dealloc(ctx, p, 8, mem);
        w32(ctx, p + pl::kAudio, 0);
    }
    w8(ctx, p + pl::kState, pl::kFinalized);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- events

s32 cellSailPlayerSubscribeEvent(PPUContext& ctx, u32 p, u32 type, u64 param)
{
    s32 r = lock(ctx, p);
    if (r < 0) return r;
    if (type < 12 && param == 0) w64(ctx, p + pl::kEventMask, r64(ctx, p + pl::kEventMask) | (u64{1} << type));
    else r = error(CELL_SAIL_ERROR_INVALID_ARG);
    unlock(ctx, p);
    return r;
}

s32 cellSailPlayerUnsubscribeEvent(PPUContext& ctx, u32 p, u32 type)
{
    s32 r = lock(ctx, p);
    if (r < 0) return r;
    if (type < 12) w64(ctx, p + pl::kEventMask, r64(ctx, p + pl::kEventMask) & ~(u64{1} << type));
    else r = error(CELL_SAIL_ERROR_INVALID_ARG);
    unlock(ctx, p);
    return r;
}

// The handler replaced, the old one and its argument returned; callable from the handler itself (the lock already
// held by its thread: EDEADLK, then no unlock).
s32 cellSailPlayerReplaceEventHandler(PPUContext& ctx, u32 p, u32 handler, u32 arg, u32 old_handler, u32 old_arg)
{
    if (!handler) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const s32 r = lock(ctx, p);
    bool locked = true;
    if (r < 0) {
        if (static_cast<u32>(r) != 0x80010008) return r;
        locked = false;
    }
    if (old_handler) w32(ctx, old_handler, r32(ctx, p + pl::kHandler));
    if (old_arg) w32(ctx, old_arg, r32(ctx, p + pl::kHandlerArg));
    w32(ctx, p + pl::kHandler, handler);
    w32(ctx, p + pl::kHandlerArg, arg);
    if (locked) unlock(ctx, p);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- getters

s32 cellSailPlayerGetDescriptorCount(PPUContext& ctx, u32 p)
{
    s32 r = lock(ctx, p);
    if (r < 0) return r;
    r = static_cast<s32>(r32(ctx, p + pl::kDescriptorCount));
    unlock(ctx, p);
    return r;
}

u32 cellSailPlayerGetCurrentDescriptor(PPUContext& ctx, u32 p) { return r32(ctx, p + pl::kCurrentDescriptor); }

s32 cellSailPlayerIsPaused(PPUContext& ctx, u32 p)
{
    s32 r = lock(ctx, p);
    if (r < 0) return r;
    r = r8(ctx, p + pl::kPaused);
    unlock(ctx, p);
    return r;
}

s32 cellSailPlayerGetRepeatMode(PPUContext& ctx, u32 p, u32 info)
{
    s32 r = lock(ctx, p);
    if (r < 0) return r;
    if (info && r8(ctx, p + pl::kRepeatSet)) std::memmove(gb(ctx) + info, gb(ctx) + p + pl::kRepeatInfo, 0x28);
    r = static_cast<s32>(r32(ctx, p + pl::kRepeatMode));
    unlock(ctx, p);
    return r;
}

s32 cellSailPlayerIsEsAudioMuted(PPUContext& ctx, u32 p, u32 index)
{
    if (index >= r32(ctx, p + pl::kAudioCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
    s32 r = lock(ctx, p);
    if (r < 0) return r;
    const u32 adapter = r32(ctx, r32(ctx, p + pl::kAudio) + index * pl::kAudioStride + 0x150);
    r = static_cast<s32>(r32(ctx, adapter + 0x34) & 1);
    unlock(ctx, p);
    return r;
}

s32 cellSailPlayerIsEsVideoMuted(PPUContext& ctx, u32 p, u32 index)
{
    if (index >= r32(ctx, p + pl::kVideoCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
    s32 r = lock(ctx, p);
    if (r < 0) return r;
    const u32 adapter = r32(ctx, r32(ctx, p + pl::kVideo) + index * pl::kVideoStride + 0x180);
    r = static_cast<s32>(r32(ctx, adapter + 0x34) & 1);
    unlock(ctx, p);
    return r;
}

// ------------------------------------------------------------------------------------------------- per-stream objects

// The audio renderer (or sound adapter, the same call in libsail) of audio stream `index`.
s32 set_audio_renderer(PPUContext& ctx, u32 p, u32 index, u32 renderer)
{
    if (r8(ctx, p + pl::kState) != pl::kInitialized) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (index >= r32(ctx, p + pl::kAudioCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
    if (!renderer) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const u32 slot = r32(ctx, p + pl::kAudio) + index * pl::kAudioStride;
    if (r32(ctx, slot + 0x150)) return error(CELL_SAIL_ERROR_USING);
    w32(ctx, slot + 0x150, renderer);
    w32(ctx, renderer + 8, index);
    w32(ctx, renderer + 0x14, p);
    w32(ctx, renderer + 0x10, p + pl::kSync);
    return lv2::CELL_OK;
}

s32 set_video_renderer(PPUContext& ctx, u32 p, u32 index, u32 renderer)
{
    if (r8(ctx, p + pl::kState) != pl::kInitialized) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (index >= r32(ctx, p + pl::kVideoCount) || !renderer)
        return error(index >= r32(ctx, p + pl::kVideoCount) ? CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE
                                                             : CELL_SAIL_ERROR_INVALID_ARG);
    const u32 slot = r32(ctx, p + pl::kVideo) + index * pl::kVideoStride;
    if (r32(ctx, slot + 0x180)) return error(CELL_SAIL_ERROR_USING);
    w32(ctx, slot + 0x180, renderer);
    w32(ctx, renderer + 8, index);
    w32(ctx, renderer + 0x14, p);
    w32(ctx, renderer + 0x10, p + pl::kSync);
    return lv2::CELL_OK;
}

s32 cellSailPlayerSetRendererAudio(PPUContext& ctx, u32 p, u32 index, u32 renderer)
{
    return set_audio_renderer(ctx, p, index, renderer);
}
s32 cellSailPlayerSetSoundAdapter(PPUContext& ctx, u32 p, u32 index, u32 adapter)
{
    return set_audio_renderer(ctx, p, index, adapter);
}
s32 cellSailPlayerSetRendererVideo(PPUContext& ctx, u32 p, u32 index, u32 renderer)
{
    return set_video_renderer(ctx, p, index, renderer);
}
s32 cellSailPlayerSetGraphicsAdapter(PPUContext& ctx, u32 p, u32 index, u32 adapter)
{
    return set_video_renderer(ctx, p, index, adapter);
}

// The AU receiver of user stream `index` (its object at the receiver's +0x20).
s32 cellSailPlayerSetAuReceiver(PPUContext& ctx, u32 p, u32 index, u32 receiver)
{
    if (r8(ctx, p + pl::kState) != pl::kInitialized) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (index >= r32(ctx, p + pl::kUserCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
    if (receiver + 0x20 == 0) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const u32 slot = r32(ctx, p + pl::kUser) + index * pl::kUserStride;
    if (r32(ctx, slot + 0x80)) return error(CELL_SAIL_ERROR_USING);
    w32(ctx, slot + 0x80, receiver + 0x20);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- parameters

// SetParameter(player, parameter, a, b): before boot only. Player-wide parameters take `a`; per-stream ones take the
// stream's index in `a` and the value in `b`; unknown ones below 36 are accepted and ignored, like libsail's table.
s32 set_audio_parameter(PPUContext& ctx, u32 slot, u32 param, u64 v)
{
    switch (param) {
    case 12: w32(ctx, slot + 0x164, static_cast<u32>(v)); break;
    case 14: w32(ctx, slot + 0x178, static_cast<u32>(v)); break;
    case 15: w64(ctx, slot + 0x170, v); break;
    case 26:
        if (v != 0) return error(CELL_SAIL_ERROR_INVALID_ARG);
        ps_write16(gb(ctx), slot + 0x156, static_cast<u16>((ps_read16(gb(ctx), slot + 0x156) & 0xFE) | 1));
        break;
    case 32:
        if (v > 1) return error(CELL_SAIL_ERROR_INVALID_ARG);
        w8(ctx, slot + 0x158, static_cast<u8>(v));
        break;
    default: break;
    }
    return lv2::CELL_OK;
}

s32 set_video_parameter(PPUContext& ctx, u32 slot, u32 param, u64 v)
{
    u8* b = gb(ctx);
    switch (param) {
    case 0:
        if (v == 0) {
            std::memset(b + slot + 0x21C, 0, 24);
            std::memset(b + slot + 0x190, 0, 96);
            w8(ctx, slot + 0x18D, 0);
            break;
        }
        w64(ctx, slot + 0x228, 0x0101010101010101ull);
        w32(ctx, slot + 0x230, 1);
        std::memset(b + slot + 0x190, 0, 96);
        {
            const u32 at = static_cast<u32>(v), kind = r32(ctx, at + 4), mode = r32(ctx, at + 8);
            if (kind > 3 || mode > 2) return error(CELL_SAIL_ERROR_INVALID_ARG);
            w32(ctx, slot + 0x190 + 8, mode);
            w32(ctx, slot + 0x190 + 4, kind);
            w8(ctx, slot + 0x18D, 1);
        }
        break;
    case 16: w32(ctx, slot + 0x1F8, static_cast<u32>(v)); break;
    case 18: w8(ctx, slot + 0x18B, static_cast<u8>(v)); break;
    case 19: w8(ctx, slot + 0x18C, static_cast<u8>(v)); break;
    case 20: w64(ctx, slot + 0x210, v); break;
    case 23: w32(ctx, slot + 0x230, static_cast<u32>(v)); break;
    case 24: w64(ctx, slot + 0x228, v); break;
    case 25:
        if (static_cast<u32>(v) - 1 > 2) return error(CELL_SAIL_ERROR_INVALID_ARG);
        w8(ctx, slot + 0x188, static_cast<u8>(v));
        break;
    case 27:
        if (v > 1) return error(CELL_SAIL_ERROR_INVALID_ARG);
        ps_write16(b, slot + 0x186, static_cast<u16>((ps_read16(b, slot + 0x186) & 0xFE) | (v == 0 ? 1 : 0)));
        break;
    case 30:
    case 31:
        if (static_cast<s32>(v) <= 0) return error(CELL_SAIL_ERROR_INVALID_ARG);
        w8(ctx, slot + (param == 30 ? 0x189 : 0x18A), static_cast<u8>(v));
        break;
    case 35:
        if (v > 1) return error(CELL_SAIL_ERROR_INVALID_ARG);
        w8(ctx, slot + 0x18E, static_cast<u8>(v));
        break;
    default: break;
    }
    return lv2::CELL_OK;
}

s32 set_file_calls(PPUContext& ctx, u32 source, u32 calls);

s32 cellSailPlayerSetParameter(PPUContext& ctx, u32 p, u32 param, u64 a, u64 v)
{
    if (r8(ctx, p + pl::kState) != pl::kInitialized) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (param > 35) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const u32 a32 = static_cast<u32>(a);
    switch (param) {
    case 1: return error(CELL_SAIL_ERROR_INVALID_ARG);
    case 2: w32(ctx, p + pl::kThreadPrio, a32); break;
    case 3: w32(ctx, p + pl::kSpursSpus, a32); break;
    case 4: w32(ctx, p + pl::kSpursSpuPrio, a32); break;
    case 5: w32(ctx, p + pl::kSpursPpuPrio, a32); break;
    case 6: w8(ctx, p + pl::kSpursExit, a != 0 ? 1 : 0); break;
    case 7: w32(ctx, p + 0x3A8, a32); break; // the file source thread's priority
    case 8: w32(ctx, p + 0x360, a32); break;
    case 10: w32(ctx, p + 0x374, a32); break;
    case 11: w64(ctx, p + 0x36C, a); break;
    case 29: w32(ctx, p + pl::kThreadStack, a32); break;
    case 33:
        if ((a || v) && (!a || v != 0x28)) return error(CELL_SAIL_ERROR_INVALID_ARG);
        return set_file_calls(ctx, r32(ctx, p + pl::kSource), a32);
    case 34: w32(ctx, p + 0x3AC, a32); break; // the file source thread's stack
    case 12: case 13: case 14: case 15: case 26: case 32:
        if (a32 >= r32(ctx, p + pl::kAudioCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        return set_audio_parameter(ctx, r32(ctx, p + pl::kAudio) + a32 * pl::kAudioStride, param, v);
    case 0: case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 27: case 30:
    case 31: case 35:
        if (a32 >= r32(ctx, p + pl::kVideoCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        return set_video_parameter(ctx, r32(ctx, p + pl::kVideo) + a32 * pl::kVideoStride, param, v);
    default: break; // 9, 28: accepted
    }
    return lv2::CELL_OK;
}

// The built-in source's file calls replaced by the game's (parameter 33): NULL puts libsail's back; else the game's
// structure {0, open, ?, close, fstat, read, lseek, ...} with every call present, its first 0x28 bytes copied.
s32 set_file_calls(PPUContext& ctx, u32 source, u32 calls)
{
    u8* b = gb(ctx);
    if (!calls) {
        const u32 own[14] = {0xFFFFFFFF, internal("_pspocket_cellSail_fsOpen"), 0, internal("_pspocket_cellSail_fsClose"),
            internal("_pspocket_cellSail_fsRead"), internal("_pspocket_cellSail_fsLseek"),
            internal("_pspocket_cellSail_fsFstat"), internal("_pspocket_cellSail_fsCancel"), 0, 0, 0, 0, 0, 0};
        for (u32 i = 0; i < 14; i++) ps_write32(b, source + fs::kCalls + 4 * i, own[i]);
        return lv2::CELL_OK;
    }
    if (r32(ctx, calls) != 0 || !r32(ctx, calls + 4) || !r32(ctx, calls + 12) || !r32(ctx, calls + 16) ||
        !r32(ctx, calls + 20) || !r32(ctx, calls + 24) || !r32(ctx, calls + 28))
        return error(CELL_SAIL_ERROR_INVALID_ARG);
    std::memmove(b + source + fs::kCalls, b + calls, 0x28);
    return lv2::CELL_OK;
}

// GetParameter(player, parameter, u64* a, u64* b): player-wide values into *a; per-stream ones by the index in *a into
// *b. Unknown ones read 0 or are INVALID_ARG as in libsail's tables.
s32 cellSailPlayerGetParameter(PPUContext& ctx, u32 p, u32 param, u32 a, u32 b)
{
    const s32 bad = error(CELL_SAIL_ERROR_INVALID_ARG);
    if (param == 0) {
        if (!a || !b) return bad;
        const u32 index = static_cast<u32>(r64(ctx, a));
        if (index >= r32(ctx, p + pl::kVideoCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        w64(ctx, b, r8(ctx, r32(ctx, p + pl::kVideo) + index * pl::kVideoStride + 0x18D));
        return lv2::CELL_OK;
    }
    if (!a) return bad;
    const auto s32_of = [&](u32 at) { return static_cast<u64>(static_cast<s64>(static_cast<s32>(r32(ctx, at)))); };
    switch (param) {
    case 1: w64(ctx, a, static_cast<u32>(r32(ctx, p + pl::kQueue) - 50)); return lv2::CELL_OK;
    case 2: w64(ctx, a, s32_of(p + pl::kThreadPrio)); return lv2::CELL_OK;
    case 3: w64(ctx, a, s32_of(p + pl::kSpursSpus)); return lv2::CELL_OK;
    case 4: w64(ctx, a, s32_of(p + pl::kSpursSpuPrio)); return lv2::CELL_OK;
    case 5: w64(ctx, a, s32_of(p + pl::kSpursPpuPrio)); return lv2::CELL_OK;
    case 6: w64(ctx, a, r8(ctx, p + pl::kSpursExit)); return lv2::CELL_OK;
    case 7: w64(ctx, a, r32(ctx, p + 0x3A8) ? s32_of(p + 0x3A8) : 0x4B0); return lv2::CELL_OK;
    case 8: w64(ctx, a, r32(ctx, p + 0x360)); return lv2::CELL_OK;
    case 9: return lv2::CELL_OK;
    case 10: w64(ctx, a, r32(ctx, p + 0x374)); return lv2::CELL_OK;
    case 11: w64(ctx, a, r64(ctx, p + 0x36C)); return lv2::CELL_OK;
    case 29: w64(ctx, a, r32(ctx, p + pl::kThreadStack)); return lv2::CELL_OK;
    case 33:
        if (!b || r64(ctx, b) != 0x28) return bad;
        std::memmove(gb(ctx) + a, gb(ctx) + r32(ctx, p + pl::kSource) + fs::kCalls, 0x28);
        return lv2::CELL_OK;
    case 34: w64(ctx, a, r32(ctx, p + 0x3AC) ? r32(ctx, p + 0x3AC) : 0x1000); return lv2::CELL_OK;
    default: break;
    }
    // The per-stream parameters (12-28, 30-32, 35).
    if (!b || param > 35) return bad;
    const u64 bit = u64{1} << param;
    const u32 index = static_cast<u32>(r64(ctx, a));
    u64 v = 0;
    if (bit & 0x8CBFF0000ull) {
        if (index >= r32(ctx, p + pl::kVideoCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        const u32 slot = r32(ctx, p + pl::kVideo) + index * pl::kVideoStride;
        switch (param) {
        case 16: v = s32_of(slot + 0x1F8); break;
        case 18: v = r8(ctx, slot + 0x18B); break;
        case 19: v = r8(ctx, slot + 0x18C); break;
        case 20: v = r64(ctx, slot + 0x210); break;
        case 23: v = r32(ctx, slot + 0x230); break;
        case 24: v = r64(ctx, slot + 0x228); break;
        case 25: v = r8(ctx, slot + 0x188); break;
        case 27: v = (ps_read16(gb(ctx), slot + 0x186) ^ 1) & 1; break;
        case 30: v = r8(ctx, slot + 0x189); break;
        case 31: v = r8(ctx, slot + 0x18A); break;
        case 35: v = static_cast<u64>(static_cast<s64>(static_cast<s8>(r8(ctx, slot + 0x18E)))); break;
        default: break;
        }
    } else if (bit & 0x10400F000ull) {
        if (index >= r32(ctx, p + pl::kAudioCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        const u32 slot = r32(ctx, p + pl::kAudio) + index * pl::kAudioStride;
        switch (param) {
        case 12: v = r32(ctx, slot + 0x164); break;
        case 14: v = r32(ctx, slot + 0x178); break;
        case 15: v = r64(ctx, slot + 0x170); break;
        case 26: v = (ps_read16(gb(ctx), slot + 0x156) ^ 1) & 1; break;
        case 32: v = r8(ctx, slot + 0x158); break;
        default: break;
        }
    } else if (!(bit & 0x10000000ull)) {
        return bad;
    }
    w64(ctx, b, v);
    return lv2::CELL_OK;
}

#define SAIL_INTERNAL(n)                                                                                               \
    ::pspocket::hle::Function{{}, "_pspocket_cellSail_" #n, recomp::nid_of("_pspocket_cellSail_" #n),                  \
        &detail::thunk<&_pspocket_cellSail_##n>, true}
#define SAIL(f) ::pspocket::hle::Function{{}, #f, recomp::nid_of(#f), &detail::thunk<&f>, false}

} // namespace

void register_cellSailPlayer(Registry& r)
{
    r.add("cellSail", {
        SAIL(cellSailPlayerInitialize),
        SAIL(cellSailPlayerInitialize2),
        SAIL(cellSailPlayerFinalize),
        SAIL(cellSailPlayerRegisterSource),
        SAIL(cellSailPlayerUnregisterSource),
        SAIL(cellSailPlayerGetRegisteredProtocols),
        SAIL(cellSailPlayerSubscribeEvent),
        SAIL(cellSailPlayerUnsubscribeEvent),
        SAIL(cellSailPlayerReplaceEventHandler),
        SAIL(cellSailPlayerGetDescriptorCount),
        SAIL(cellSailPlayerGetCurrentDescriptor),
        SAIL(cellSailPlayerIsPaused),
        SAIL(cellSailPlayerGetRepeatMode),
        SAIL(cellSailPlayerIsEsAudioMuted),
        SAIL(cellSailPlayerIsEsVideoMuted),
        SAIL(cellSailPlayerSetRendererAudio),
        SAIL(cellSailPlayerSetSoundAdapter),
        SAIL(cellSailPlayerSetRendererVideo),
        SAIL(cellSailPlayerSetGraphicsAdapter),
        SAIL(cellSailPlayerSetAuReceiver),
        SAIL(cellSailPlayerSetParameter),
        SAIL(cellSailPlayerGetParameter),
        SAIL_INTERNAL(fsOpen),
        SAIL_INTERNAL(fsClose),
        SAIL_INTERNAL(fsRead),
        SAIL_INTERNAL(fsLseek),
        SAIL_INTERNAL(fsFstat),
        SAIL_INTERNAL(fsCancel),
        SAIL_INTERNAL(srcMakeup),
        SAIL_INTERNAL(srcCleanup),
        SAIL_INTERNAL(srcOpen),
        SAIL_INTERNAL(srcClose),
        SAIL_INTERNAL(srcStart),
        SAIL_INTERNAL(srcStop),
        SAIL_INTERNAL(srcCancel),
        SAIL_INTERNAL(srcCheckout),
        SAIL_INTERNAL(srcCheckin),
        SAIL_INTERNAL(srcClear),
        SAIL_INTERNAL(srcRead),
        SAIL_INTERNAL(srcReadSync),
        SAIL_INTERNAL(srcGetCapabilities),
        SAIL_INTERNAL(srcInquireCapability),
    });
}
#undef SAIL
#undef SAIL_INTERNAL

} // namespace pspocket::hle
