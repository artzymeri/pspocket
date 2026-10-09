// cellSail's sound and graphics adapters (D-562, part 4a): libsail's own renderers that the game reads frames from
// (GetFrame) instead of writing its own renderer. What does not need playback, as libsail (fw 4.92) has it:
// - Sound adapter (0xD8 bytes): a CellSailRendererAudio (its callbacks libsail's own) with +0x50 the game's 3 callbacks
//   and +0x5C their argument, +0x60 an lwmutex "_saisa0", +0x78 its state (7 initialized, 6 finalized), +0x90 the
//   format it delivers, +0xC0 its AV-sync record, +0xD0 the first PTS (-1 none).
// - Graphics adapter (0x100 bytes): a CellSailRendererVideo with +0x60 the game's 5 callbacks and +0x74 their argument,
//   +0x78 an lwmutex "_saiga0", +0x90 its state, +0x98 the first PTS, +0xA8 the format it delivers, +0xDC its AV-sync
//   record.
// UpdateAvSync feeds the player's clock: the audio and video halves measure the time per frame from the game's (frame,
// time) reports, smoothed by a quarter. The adapters' renderer callbacks run once the player plays (a later part);
// until then they fail loudly.

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

namespace sa {
constexpr u32 kFuncs = 0x50, kArg = 0x5C, kMutex = 0x60, kState = 0x78, kFormat = 0x90, kSync = 0xC0, kFirstPts = 0xD0,
              kSize = 0xD8, kPreferred = 0x38;
}
namespace ga {
constexpr u32 kFuncs = 0x60, kArg = 0x74, kMutex = 0x78, kState = 0x90, kFirstPts = 0x98, kFormat = 0xA8, kSync = 0xDC,
              kSize = 0x100, kPreferred = 0x40;
}
constexpr u32 kInitialized = 7, kFinalized = 6;

// ------------------------------------------------------------------------------------------------- renderer callbacks

void not_yet(PPUContext& ctx, std::string_view name)
{
    log_call_site(ctx, gb(ctx), std::format("cellSail: {} (an adapter's renderer callback) is not pspocket's yet", name));
    PS_FATAL(kChan, "{}: libsail's adapters play in a later part of D-562", name);
}

#define SAIL_ADAPTER_STUB(n)                                                                                           \
    s32 _pspocket_cellSail_##n(PPUContext& ctx)                                                                        \
    {                                                                                                                  \
        not_yet(ctx, #n);                                                                                              \
        return error(CELL_SAIL_ERROR_FATAL);                                                                           \
    }
SAIL_ADAPTER_STUB(saMakeup)
SAIL_ADAPTER_STUB(saCleanup)
SAIL_ADAPTER_STUB(saOpen)
SAIL_ADAPTER_STUB(saClose)
SAIL_ADAPTER_STUB(saStart)
SAIL_ADAPTER_STUB(saStop)
SAIL_ADAPTER_STUB(saCancel)
SAIL_ADAPTER_STUB(saCheckout)
SAIL_ADAPTER_STUB(saCheckin)
SAIL_ADAPTER_STUB(gaMakeup)
SAIL_ADAPTER_STUB(gaCleanup)
SAIL_ADAPTER_STUB(gaOpen)
SAIL_ADAPTER_STUB(gaClose)
SAIL_ADAPTER_STUB(gaStart)
SAIL_ADAPTER_STUB(gaStop)
SAIL_ADAPTER_STUB(gaCancel)
SAIL_ADAPTER_STUB(gaCheckout)
SAIL_ADAPTER_STUB(gaCheckin)
SAIL_ADAPTER_STUB(auMakeup)
SAIL_ADAPTER_STUB(auCleanup)
SAIL_ADAPTER_STUB(auOpen)
SAIL_ADAPTER_STUB(auClose)
SAIL_ADAPTER_STUB(auStart)
SAIL_ADAPTER_STUB(auStop)
SAIL_ADAPTER_STUB(auCancel)
SAIL_ADAPTER_STUB(auCheckout)
SAIL_ADAPTER_STUB(auCheckin)
#undef SAIL_ADAPTER_STUB

u32 internal(std::string_view name) { return HleProcess::current()->link_function("cellSail", recomp::nid_of(name), name); }

// The adapters' renderer callback tables (libsail's data): 9 descriptors each.
void init_sound_table(u8* b, u32 at)
{
    static constexpr std::string_view kNames[9] = {"_pspocket_cellSail_saMakeup", "_pspocket_cellSail_saCleanup",
        "_pspocket_cellSail_saOpen", "_pspocket_cellSail_saClose", "_pspocket_cellSail_saStart",
        "_pspocket_cellSail_saStop", "_pspocket_cellSail_saCancel", "_pspocket_cellSail_saCheckout",
        "_pspocket_cellSail_saCheckin"};
    for (u32 i = 0; i < 9; i++) ps_write32(b, at + 4 * i, internal(kNames[i]));
}

void init_graphics_table(u8* b, u32 at)
{
    static constexpr std::string_view kNames[9] = {"_pspocket_cellSail_gaMakeup", "_pspocket_cellSail_gaCleanup",
        "_pspocket_cellSail_gaOpen", "_pspocket_cellSail_gaClose", "_pspocket_cellSail_gaStart",
        "_pspocket_cellSail_gaStop", "_pspocket_cellSail_gaCancel", "_pspocket_cellSail_gaCheckout",
        "_pspocket_cellSail_gaCheckin"};
    for (u32 i = 0; i < 9; i++) ps_write32(b, at + 4 * i, internal(kNames[i]));
}

// The AU receiver's (9 descriptors, copied into each receiver).
void init_receiver_table(u8* b, u32 at)
{
    static constexpr std::string_view kNames[9] = {"_pspocket_cellSail_auMakeup", "_pspocket_cellSail_auCleanup",
        "_pspocket_cellSail_auOpen", "_pspocket_cellSail_auClose", "_pspocket_cellSail_auStart",
        "_pspocket_cellSail_auStop", "_pspocket_cellSail_auCancel", "_pspocket_cellSail_auCheckout",
        "_pspocket_cellSail_auCheckin"};
    for (u32 i = 0; i < 9; i++) ps_write32(b, at + 4 * i, internal(kNames[i]));
}

u32 table(std::string_view name)
{
    return HleProcess::current()->link_variable("cellSail", recomp::nid_of(name));
}

// ------------------------------------------------------------------------------------------------- helpers

s32 lwmutex_create(PPUContext& ctx, u32 at, const char* name)
{
    u8* b = gb(ctx);
    StackVar v(ctx, b, 16);
    ps_write32(b, v.addr(), 2);
    ps_write32(b, v.addr() + 4, 0x20);
    std::memcpy(b + v.addr() + 8, name, 8);
    return static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_create", {at, v.addr()}));
}

s32 lwmutex_destroy(PPUContext& ctx, u32 at) { return static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_destroy", {at})); }

s32 renderer_initialize(PPUContext& ctx, bool video, u32 r, u32 funcs, u32 arg, u32 attr)
{
    const char* name = video ? "cellSailRendererVideoInitialize" : "cellSailRendererAudioInitialize";
    const Function* f = registry().find("cellSail", recomp::nid_of(name));
    u64 saved[4] = {ctx.gpr[3], ctx.gpr[4], ctx.gpr[5], ctx.gpr[6]};
    ctx.gpr[3] = r;
    ctx.gpr[4] = funcs;
    ctx.gpr[5] = arg;
    ctx.gpr[6] = attr;
    f->fn(ctx, gb(ctx));
    const s32 res = static_cast<s32>(ctx.gpr[3]);
    for (u32 i = 0; i < 4; i++) ctx.gpr[3 + i] = saved[i];
    return res;
}

// The adapter's lwmutex destroyed, again every `wait` microseconds while busy, ten times at most.
void destroy_retrying(PPUContext& ctx, u32 at, u32 wait)
{
    for (u32 n = 0; n < 10; n++) {
        if (static_cast<u32>(lwmutex_destroy(ctx, at)) != 0x8001000A) return;
        sc(ctx, 141, {wait});
    }
}

// ------------------------------------------------------------------------------------------------- sound adapter

s32 cellSailSoundAdapterInitialize(PPUContext& ctx, u32 a, u32 funcs, u32 arg)
{
    std::memset(gb(ctx) + a, 0, sa::kSize);
    s32 r = lwmutex_create(ctx, a + sa::kMutex, "_saisa0");
    if (r < 0) return r;
    r = renderer_initialize(ctx, false, a, table("_pspocket_cellSailSoundAdapterRendererFuncs"), a, 0);
    if (r < 0) {
        lwmutex_destroy(ctx, a + sa::kMutex);
        return r;
    }
    w64(ctx, a + sa::kFirstPts, ~u64{0});
    std::memmove(gb(ctx) + a + sa::kFuncs, gb(ctx) + funcs, 12);
    w32(ctx, a + sa::kArg, arg);
    w32(ctx, a + sa::kState, kInitialized);
    return lv2::CELL_OK;
}

s32 cellSailSoundAdapterFinalize(PPUContext& ctx, u32 a)
{
    w32(ctx, a + 0x1C, 9); // cellSailRendererAudioFinalize
    destroy_retrying(ctx, a + sa::kMutex, 5000);
    w32(ctx, a + sa::kState, kFinalized);
    return lv2::CELL_OK;
}

s32 cellSailSoundAdapterGetFormat(PPUContext& ctx, u32 a, u32 out)
{
    std::memmove(gb(ctx) + out, gb(ctx) + a + sa::kFormat, 0x18);
    w16(ctx, out + 2, 0x100);
    return lv2::CELL_OK;
}

s32 cellSailSoundAdapterSetPreferredFormat(PPUContext& ctx, u32 a, u32 format)
{
    if (!format) return error(CELL_SAIL_ERROR_INVALID_ARG);
    std::memmove(gb(ctx) + a + sa::kPreferred, gb(ctx) + format, 0x18);
    return lv2::CELL_OK;
}

// The PTS as milliseconds from the first one (90 kHz ticks).
s32 pts_to_time(PPUContext& ctx, u32 first_at, s64 pts, u32 out)
{
    const s64 first = static_cast<s64>(r64(ctx, first_at));
    if (first == -1) return error(CELL_SAIL_ERROR_INVALID_STATE);
    w64(ctx, out, static_cast<u64>(((pts - first) * 1000) / 90));
    return lv2::CELL_OK;
}

s32 cellSailSoundAdapterPtsToTimePosition(PPUContext& ctx, u32 a, s64 pts, u32 out)
{
    return pts_to_time(ctx, a + sa::kFirstPts, pts, out);
}

// The clock's audio half (libsail's): from the game's (frame number, time in microseconds) the time per frame, in
// 1/256 frames where the clock was made with a latency; the first estimate after 100 ms, later ones smoothed by a
// quarter when within a quarter of the last.
void avsync_audio(PPUContext& ctx, u32 c, u32 frame, u64 time, u32 record, u32 index)
{
    if (!frame) return;
    lv2_lib(ctx, "sys_lwmutex_lock", {c + 0xB0, 0});
    const u32 mark = r32(ctx, record);
    bool keep = true;
    if (r32(ctx, c + 8) == 0) {
        if (r64(ctx, c + 0x18) == 0) {
            if (r64(ctx, c + 0x10) != 0) {
                const u64 dt = time - r64(ctx, c + 0x10);
                if (dt < 100000) {
                    keep = false;
                } else {
                    w32(ctx, c + 8, 1);
                    const u64 per = (dt * 0x100) / static_cast<u32>(frame - r32(ctx, c + 0xC));
                    w64(ctx, c + 0x18, per);
                    w64(ctx, c + 0x20, static_cast<u64>(static_cast<s64>(per * r64(ctx, c)) / 1000));
                }
            }
        } else {
            w32(ctx, c + 8, 1);
        }
    } else {
        const u32 frames = frame - r32(ctx, c + 0xC);
        const u64 dt = time - r64(ctx, c + 0x10);
        const u64 per = r32(ctx, c + 0x80) == 0 ? dt / frames : (dt * 0x100) / frames;
        const u64 last = r64(ctx, c + 0x18);
        if (last <= per + (last >> 2) && per <= last + (last >> 2)) {
            const u64 smooth = (per + last * 3) >> 2;
            w64(ctx, c + 0x18, smooth);
            w32(ctx, c + 8, 2);
            w64(ctx, c + 0x50, time + smooth * static_cast<u32>(mark - frame));
            w64(ctx, c + 0x20, static_cast<u64>(static_cast<s64>(smooth * r64(ctx, c)) / 1000));
        }
    }
    if (keep) {
        w32(ctx, c + 0x48, index);
        w64(ctx, c + 0x10, time);
        w32(ctx, c + 0xC, frame);
    }
    lv2_lib(ctx, "sys_lwmutex_unlock", {c + 0xB0});
}

void cellSailSoundAdapterUpdateAvSync(PPUContext& ctx, u32 a, u32 frame, u64 time)
{
    avsync_audio(ctx, r32(ctx, a + 0x10), frame, time, a + sa::kSync, r32(ctx, a + 8));
}

// ------------------------------------------------------------------------------------------------- graphics adapter

s32 cellSailGraphicsAdapterInitialize(PPUContext& ctx, u32 a, u32 funcs, u32 arg)
{
    std::memset(gb(ctx) + a, 0, ga::kSize);
    s32 r = lwmutex_create(ctx, a + ga::kMutex, "_saiga0");
    if (r < 0) return r;
    r = renderer_initialize(ctx, true, a, table("_pspocket_cellSailGraphicsAdapterRendererFuncs"), a, 0);
    if (r < 0) {
        lwmutex_destroy(ctx, a + ga::kMutex);
        return r;
    }
    w64(ctx, a + ga::kFirstPts, ~u64{0});
    std::memmove(gb(ctx) + a + ga::kFuncs, gb(ctx) + funcs, 20);
    w32(ctx, a + ga::kArg, arg);
    w32(ctx, a + ga::kState, kInitialized);
    return lv2::CELL_OK;
}

s32 cellSailGraphicsAdapterFinalize(PPUContext& ctx, u32 a)
{
    w32(ctx, a + 0x1C, 9); // cellSailRendererVideoFinalize
    destroy_retrying(ctx, a + ga::kMutex, 5000);
    w32(ctx, a + ga::kState, kFinalized);
    return lv2::CELL_OK;
}

s32 cellSailGraphicsAdapterGetFormat(PPUContext& ctx, u32 a, u32 out)
{
    std::memmove(gb(ctx) + out, gb(ctx) + a + ga::kFormat, 0x20);
    return lv2::CELL_OK;
}

s32 cellSailGraphicsAdapterSetPreferredFormat(PPUContext& ctx, u32 a, u32 format)
{
    if (!format) return error(CELL_SAIL_ERROR_INVALID_ARG);
    w64(ctx, a + ga::kFirstPts, ~u64{0});
    std::memmove(gb(ctx) + a + ga::kPreferred, gb(ctx) + format, 0x20);
    return lv2::CELL_OK;
}

s32 cellSailGraphicsAdapterPtsToTimePosition(PPUContext& ctx, u32 a, s64 pts, u32 out)
{
    return pts_to_time(ctx, a + ga::kFirstPts, pts, out);
}

// The clock's video half: the time per frame from the game's reports, the first after 8 frames, later ones kept when
// from 15001 to 149999 microseconds (libsail's window: per - 0x3A99 below 0x20F57) and smoothed by a quarter.
void avsync_video(PPUContext& ctx, u32 c, u32 frame, u64 time, u32 record, u32 index)
{
    if (!frame) return;
    lv2_lib(ctx, "sys_lwmutex_lock", {c + 0xB0, 0});
    const u32 mark = r32(ctx, record);
    bool keep = true;
    if (r32(ctx, c + 0x28) == 0) {
        if (r64(ctx, c + 0x38) == 0) {
            if (r64(ctx, c + 0x30) != 0) {
                const u32 frames = frame - r32(ctx, c + 0x2C);
                if (frames < 8) {
                    keep = false;
                } else {
                    w32(ctx, c + 0x28, 1);
                    const u64 per = (time - r64(ctx, c + 0x30)) / frames;
                    w64(ctx, c + 0x38, per);
                    w64(ctx, c + 0x40, static_cast<u64>(static_cast<s64>(per * r64(ctx, c)) / 1000));
                }
            }
        } else {
            w32(ctx, c + 0x28, 1);
        }
    } else {
        const u64 per = (time - r64(ctx, c + 0x30)) / static_cast<u32>(frame - r32(ctx, c + 0x2C));
        if (per - 0x3A99 < 0x20F57) {
            const u64 smooth = (per + r64(ctx, c + 0x38) * 3) >> 2;
            w64(ctx, c + 0x38, smooth);
            w32(ctx, c + 0x28, 2);
            w64(ctx, c + 0x60, time + smooth * static_cast<u32>(mark - frame));
            w64(ctx, c + 0x40, static_cast<u64>(static_cast<s64>(smooth * r64(ctx, c)) / 1000));
        }
    }
    if (keep) {
        w32(ctx, c + 0x4C, index);
        w64(ctx, c + 0x30, time);
        w32(ctx, c + 0x2C, frame);
    }
    lv2_lib(ctx, "sys_lwmutex_unlock", {c + 0xB0});
}

void cellSailGraphicsAdapterUpdateAvSync(PPUContext& ctx, u32 a, u32 frame, u64 time)
{
    avsync_video(ctx, r32(ctx, a + 0x10), frame, time, a + ga::kSync, r32(ctx, a + 8));
}

// ------------------------------------------------------------------------------------------------- AU receiver

// The AU receiver (0x200 bytes): +0 its state (1 initialized), +4 the argument, +8 an lwmutex "_saiau0", +0x20 its
// stream callbacks (libsail's 9, copied) and +0x44 itself, +0xE8 the second argument. Both of libsail's optional
// arguments must be NULL: fw 4.92 takes no game callbacks here.
s32 cellSailAuReceiverInitialize(PPUContext& ctx, u32 a, u32 funcs, u32 funcs_arg, u32 arg)
{
    std::memset(gb(ctx) + a, 0, 0x200);
    if (funcs | funcs_arg) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const s32 r = lwmutex_create(ctx, a + 8, "_saiau0");
    if (r < 0) return r;
    std::memmove(gb(ctx) + a + 0x20, gb(ctx) + table("_pspocket_cellSailAuReceiverFuncs"), 0x24);
    w32(ctx, a + 0x44, a);
    w32(ctx, a, 1);
    w32(ctx, a + 0xE8, funcs_arg);
    w32(ctx, a + 4, arg);
    return lv2::CELL_OK;
}

s32 cellSailAuReceiverFinalize(PPUContext& ctx, u32 a)
{
    w32(ctx, a + 0x20 + 0x38, 8);
    lwmutex_destroy(ctx, a + 8);
    std::memset(gb(ctx) + a, 0, 0x200);
    return lv2::CELL_OK;
}

} // namespace

void register_cellSailAdapters(Registry& r)
{
#define SAIL(f) ::pspocket::hle::Function{{}, #f, recomp::nid_of(#f), &detail::thunk<&f>, false}
#define SAIL_INTERNAL(n)                                                                                               \
    ::pspocket::hle::Function{{}, "_pspocket_cellSail_" #n, recomp::nid_of("_pspocket_cellSail_" #n),                  \
        &detail::thunk<&_pspocket_cellSail_##n>, true}
    r.add("cellSail", {
        SAIL(cellSailSoundAdapterInitialize),
        SAIL(cellSailSoundAdapterFinalize),
        SAIL(cellSailSoundAdapterGetFormat),
        SAIL(cellSailSoundAdapterSetPreferredFormat),
        SAIL(cellSailSoundAdapterPtsToTimePosition),
        SAIL(cellSailSoundAdapterUpdateAvSync),
        SAIL(cellSailGraphicsAdapterInitialize),
        SAIL(cellSailGraphicsAdapterFinalize),
        SAIL(cellSailGraphicsAdapterGetFormat),
        SAIL(cellSailGraphicsAdapterSetPreferredFormat),
        SAIL(cellSailGraphicsAdapterPtsToTimePosition),
        SAIL(cellSailGraphicsAdapterUpdateAvSync),
        SAIL(cellSailAuReceiverInitialize),
        SAIL(cellSailAuReceiverFinalize),
        SAIL_INTERNAL(auMakeup),
        SAIL_INTERNAL(auCleanup),
        SAIL_INTERNAL(auOpen),
        SAIL_INTERNAL(auClose),
        SAIL_INTERNAL(auStart),
        SAIL_INTERNAL(auStop),
        SAIL_INTERNAL(auCancel),
        SAIL_INTERNAL(auCheckout),
        SAIL_INTERNAL(auCheckin),
        SAIL_INTERNAL(saMakeup),
        SAIL_INTERNAL(saCleanup),
        SAIL_INTERNAL(saOpen),
        SAIL_INTERNAL(saClose),
        SAIL_INTERNAL(saStart),
        SAIL_INTERNAL(saStop),
        SAIL_INTERNAL(saCancel),
        SAIL_INTERNAL(saCheckout),
        SAIL_INTERNAL(saCheckin),
        SAIL_INTERNAL(gaMakeup),
        SAIL_INTERNAL(gaCleanup),
        SAIL_INTERNAL(gaOpen),
        SAIL_INTERNAL(gaClose),
        SAIL_INTERNAL(gaStart),
        SAIL_INTERNAL(gaStop),
        SAIL_INTERNAL(gaCancel),
        SAIL_INTERNAL(gaCheckout),
        SAIL_INTERNAL(gaCheckin),
    });
    r.add_variables("cellSail", {
        ::pspocket::hle::Variable{{}, "_pspocket_cellSailSoundAdapterRendererFuncs",
            recomp::nid_of("_pspocket_cellSailSoundAdapterRendererFuncs"), 36, 4, &init_sound_table},
        ::pspocket::hle::Variable{{}, "_pspocket_cellSailGraphicsAdapterRendererFuncs",
            recomp::nid_of("_pspocket_cellSailGraphicsAdapterRendererFuncs"), 36, 4, &init_graphics_table},
        ::pspocket::hle::Variable{{}, "_pspocket_cellSailAuReceiverFuncs",
            recomp::nid_of("_pspocket_cellSailAuReceiverFuncs"), 36, 4, &init_receiver_table},
    });
#undef SAIL_INTERNAL
#undef SAIL
}

} // namespace pspocket::hle
