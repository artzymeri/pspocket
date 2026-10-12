// cellSailRec's audio streams (D-918, part 7), as libsail_rec (fw 4.92) runs them on its control thread: a stream's
// configuration once its feeder is open (the format the feeder gives against the profile's parameters), its pipeline
// (the feeder's frames mixed down to stereo where they have 6 or 8 channels, resampled by the firmware's
// cellApostSrcMini where the profile asks 24 or 48 kHz of another rate, encoded to AAC), and the access units handed
// to the composer.
//
// The encoder is libsail_rec's own AAC library ("libaacenc"): an SPU task of its own taskset on the game's SPURS
// (libsail_rec's SPU program, read from the installed module as the converters' are, with its overlays through
// cellOvis and its heap through cellSheap), driven through two SPURS queues of one 16-byte message: a command
// {code, a, b} pushed, its answer {.., .., b, result} popped; the encoding commands answer later, the task telling
// the recorder (its SPURS queue, message 0x16) when a unit is ready. The resampler tells it with message 0x15.
//
// The pipeline (a slot's +0x50, 0x228 bytes, ap:: below): states 1 made, 2 open, 3 started, 4 stopping (the last
// frame's samples padded), 5 resampling the end, 6 and 7 the resampler drained, 8 the input's end, 9 the encoder
// flushed, 10 ended (its stop completes once the composer has given every unit back). The input goes, a feeder frame
// at a time (the feeder's checkout through the pump), into an intermediate buffer of one encoder frame (or straight to
// the encoder when it needs no change), then to the encoder, whose units go into a ring of the block the pipeline
// allocates, each with a record in a table; the composer takes them as it has room (message 0x18) and gives them back
// (0x2B).

#include <bit>
#include <cmath>
#include <cstring>
#include <format>

#include "cellSailRec.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/ppu/fpu.h"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::sailrec {

namespace {

s32 sx(u64 v) { return static_cast<s32>(static_cast<u32>(v)); }

s32 spurs(PPUContext& ctx, std::string_view name, std::initializer_list<u64> args)
{
    return sx(sail::module_call(ctx, "cellSpurs", name, args));
}

// ------------------------------------------------------------------------------------------------- libaacenc

// The encoder (in the pipeline at +0x88): +0 1 when the library made its own SPURS, +4 the SPURS, +8 the taskset,
// +0xC and +0x10 the command and answer queues, +0x14 the task's ID, +0x18 the task's arguments (the queues, the I/O
// buffer and the overlay table, as u64), +0x1C the task's context save area, +0x20 the message both queues use (+0x40
// 1 once open), +0x24 the taskset's name, +0x5C the I/O buffer (0x2000 bytes), +0x60 the SPU heap (0x40000 bytes),
// +0x64 the overlay table. All of it in the work area the caller gives (1 MB, laid out at 128 bytes).
namespace ae {
constexpr u32 kOwnSpurs = 0, kSpurs = 4, kTaskset = 8, kCommands = 0xC, kAnswers = 0x10, kTaskId = 0x14, kArgs = 0x18,
              kContext = 0x1C, kMessage = 0x20, kName = 0x24, kIo = 0x5C, kHeap = 0x60, kOverlays = 0x64;
}
constexpr s32 kAacInvalid = static_cast<s32>(0x80614301), kAacState = static_cast<s32>(0x80614307),
              kAacFatal = static_cast<s32>(0x806143FF);
// libsail_rec's encoder: its SPU program (an ELF) and the task's local storage pattern.
constexpr u32 kAacProgram = 0x2D600, kAacPattern = 0x1CD10;

u32 align(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }
u32 msg(PPUContext& ctx, u32 e) { return r32(ctx, e + ae::kMessage); }

s32 push(PPUContext& ctx, u32 e) { return spurs(ctx, "cellSpursQueuePushBody", {r32(ctx, e + ae::kCommands), msg(ctx, e), 1}); }
s32 pop(PPUContext& ctx, u32 e) { return spurs(ctx, "cellSpursQueuePopBody", {r32(ctx, e + ae::kAnswers), msg(ctx, e), 0, 1}); }

void command(PPUContext& ctx, u32 e, u32 code, u32 a, u32 b)
{
    const u32 m = msg(ctx, e);
    w32(ctx, m, code);
    w32(ctx, m + 4, a);
    w32(ctx, m + 8, b);
}

// A command answered at once: its result (the answer's last word), or 0x806143FF when a queue fails.
s32 ask(PPUContext& ctx, u32 e, u32 code, u32 a, u32 b)
{
    command(ctx, e, code, a, b);
    if (push(ctx, e) != 0 || pop(ctx, e) != 0) return kAacFatal;
    return static_cast<s32>(r32(ctx, msg(ctx, e) + 0xC));
}

u32 aac_elf(PPUContext& ctx) { return spu_program(ctx, kAacProgram); }

// The work area the library needs.
u32 aac_work_size(PPUContext& ctx)
{
    u32 size = 0x2080 + 0x40080;
    size += static_cast<u32>(sail::module_call(ctx, "cellOvis", "cellOvisGetOverlayTableSize", {aac_elf(ctx)})) + 0x80;
    return size + 0x1080 + 0x2980 + 0x100 + 0x100 + 0xA0 + 0x3DC80 + 0x20;
}

// The library made in `work`: laid out, its SPURS (the given one, or one of its own), the heap and the overlays, the
// taskset ("_libaacenc_<the encoder>") and its two queues, the task; then a first empty command. The SPU priorities
// and maximum contention are the taskset's.
s32 aac_init(PPUContext& ctx, u32 e, u32 spu_priority, u32 work, u32 own_spurs, u32 priorities, u32 contention)
{
    if (!e || !work) return kAacInvalid;
    w8(ctx, e + ae::kOwnSpurs, 0);
    const u32 size = aac_work_size(ctx);
    if (static_cast<s32>(size) > 0) std::memset(gb(ctx) + work, 0, size);
    const u32 elf = aac_elf(ctx);
    u32 a = align(work, 128);
    w32(ctx, e + ae::kIo, a);
    a = align(a + 0x2000, 128);
    w32(ctx, e + ae::kHeap, a);
    a = align(a + 0x40000, 128);
    w32(ctx, e + ae::kOverlays, a);
    a += static_cast<u32>(sail::module_call(ctx, "cellOvis", "cellOvisGetOverlayTableSize", {elf}));
    if (!own_spurs) {
        w8(ctx, e + ae::kOwnSpurs, 1);
        a = align(a, 128);
        w32(ctx, e + ae::kSpurs, a);
        a += 0x1000;
    } else {
        w32(ctx, e + ae::kSpurs, own_spurs);
    }
    a = align(a, 128);
    w32(ctx, e + ae::kTaskset, a);
    a = align(a + 0x2900, 128);
    w32(ctx, e + ae::kCommands, a);
    a = align(a + 0x80, 128);
    w32(ctx, e + ae::kAnswers, a);
    a = align(a + 0x80, 128);
    w32(ctx, e + ae::kContext, a);
    a = align(a + 0x3DC00, 128);
    w32(ctx, e + ae::kArgs, a);
    a = align(a + 0x20, 16);
    w32(ctx, e + ae::kMessage, a);
    const u32 end = a + 0x10;
    sail::module_call(ctx, "cellSheap", "cellSheapInitialize", {r32(ctx, e + ae::kHeap), 0x40000, 5});
    if (r8(ctx, e + ae::kOwnSpurs)) {
        // The calling thread's priority less one for the SPURS' PPU threads.
        StackVar id(ctx, gb(ctx), 16);
        if (sail::lv2_lib(ctx, "sys_ppu_thread_get_id", {id.addr()}) != 0) return kAacFatal;
        StackVar prio(ctx, gb(ctx), 16);
        if (sc(ctx, 48, {r64(ctx, id.addr()), prio.addr()}) != 0) return kAacFatal;
        const u32 ppu_priority = r32(ctx, prio.addr()) - 1;
        StackVar attr(ctx, gb(ctx), 0x200);
        if (spurs(ctx, "_cellSpursAttributeInitialize",
                {attr.addr(), 2, 0x330000, 1, sx32(spu_priority), sx32(ppu_priority), 1}) != 0)
            return kAacFatal;
        if (spurs(ctx, "cellSpursInitializeWithAttribute", {r32(ctx, e + ae::kSpurs), attr.addr()}) != 0) return kAacFatal;
    }
    if (sx(sail::module_call(ctx, "cellOvis", "cellOvisInitializeOverlayTable", {r32(ctx, e + ae::kOverlays), elf})) != 0)
        return kAacFatal;
    const u32 args = r32(ctx, e + ae::kArgs);
    w64(ctx, args, r32(ctx, e + ae::kCommands));
    w64(ctx, args + 8, r32(ctx, e + ae::kAnswers));
    w64(ctx, args + 0x10, r32(ctx, e + ae::kIo));
    w64(ctx, args + 0x18, r32(ctx, e + ae::kOverlays));
    StackVar attr(ctx, gb(ctx), 0x200);
    if (spurs(ctx, "_cellSpursTasksetAttributeInitialize", {attr.addr(), 1, 0x330000, args, priorities, contention}) != 0)
        return kAacFatal;
    const std::string name = std::format("_libaacenc_{:08x}", e);
    std::memcpy(gb(ctx) + e + ae::kName, name.c_str(), name.size() + 1);
    if (spurs(ctx, "cellSpursTasksetAttributeSetName", {attr.addr(), e + ae::kName}) != 0) return kAacFatal;
    if (spurs(ctx, "cellSpursTasksetAttributeSetTasksetSize", {attr.addr(), 0x2900}) != 0) return kAacFatal;
    if (spurs(ctx, "cellSpursCreateTasksetWithAttribute", {r32(ctx, e + ae::kSpurs), r32(ctx, e + ae::kTaskset), attr.addr()}))
        return kAacFatal;
    const u32 taskset = r32(ctx, e + ae::kTaskset), m = msg(ctx, e);
    if (spurs(ctx, "_cellSpursQueueInitialize", {0, taskset, r32(ctx, e + ae::kCommands), m, 16, 1, 2}) != 0 ||
        spurs(ctx, "cellSpursQueueAttachLv2EventQueue", {r32(ctx, e + ae::kCommands)}) != 0 ||
        spurs(ctx, "_cellSpursQueueInitialize", {0, taskset, r32(ctx, e + ae::kAnswers), m, 16, 1, 1}) != 0 ||
        spurs(ctx, "cellSpursQueueAttachLv2EventQueue", {r32(ctx, e + ae::kAnswers)}) != 0)
        return kAacFatal;
    if (spurs(ctx, "cellSpursCreateTask", {taskset, e + ae::kTaskId, elf, r32(ctx, e + ae::kContext), 0x3DC00,
                                              module_copy(ctx, kAacPattern, 16), 0}) != 0)
        return kAacFatal;
    w8(ctx, end, 0);
    return ask(ctx, e, 0, 0, 0);
}

// Open: made, then told where its units' notifications go (`notify`: {SPURS queue, code, argument, 0}).
s32 aac_open(PPUContext& ctx, u32 e, u32 spu_priority, u32 work, u32 own_spurs, u32 notify, u32 priorities, u32 contention)
{
    if (const s32 r = aac_init(ctx, e, spu_priority, work, own_spurs, priorities, contention)) return r;
    if (!notify) return kAacInvalid;
    const u32 m = msg(ctx, e);
    command(ctx, e, 0x15, 0x80, 0);
    std::memmove(gb(ctx) + r32(ctx, e + ae::kIo), gb(ctx) + notify, 16);
    if (push(ctx, e) != 0 || pop(ctx, e) != 0) return kAacFatal;
    const s32 r = static_cast<s32>(r32(ctx, m + 0xC));
    if (r == 0) w8(ctx, m + 0x40, 1);
    return r;
}

// Close: the task told to end, the queues detached, the taskset shut down and joined (and its own SPURS finalized).
s32 aac_close(PPUContext& ctx, u32 e)
{
    command(ctx, e, 1, 0, 0);
    if (push(ctx, e) != 0 || pop(ctx, e) != 0) return kAacFatal;
    if (spurs(ctx, "cellSpursQueueDetachLv2EventQueue", {r32(ctx, e + ae::kCommands)}) != 0 ||
        spurs(ctx, "cellSpursQueueDetachLv2EventQueue", {r32(ctx, e + ae::kAnswers)}) != 0 ||
        spurs(ctx, "cellSpursShutdownTaskset", {r32(ctx, e + ae::kTaskset)}) != 0 ||
        spurs(ctx, "cellSpursJoinTaskset", {r32(ctx, e + ae::kTaskset)}) != 0)
        return kAacFatal;
    if (r8(ctx, e + ae::kOwnSpurs) && spurs(ctx, "cellSpursFinalize", {r32(ctx, e + ae::kSpurs)}) != 0) return kAacFatal;
    return 0;
}

// The configuration (0x80 bytes: channels 1 or 2, the sample rate, the bit rate, ... and three tables), checked as the
// library does, copied with its tables into the I/O buffer and sent.
s32 aac_configure(PPUContext& ctx, u32 e, u32 c)
{
    const u32 io = r32(ctx, e + ae::kIo);
    if (!e || !c) return kAacInvalid;
    const auto w = [&](u32 i) { return static_cast<s32>(r32(ctx, c + 4 * i)); };
    const auto flag = [&](u32 i) { return w(i) == 0 || w(i) == 1; };
    const s32 ch = w(0);
    if (ch <= 0 || ch > 2) return kAacInvalid;
    const double per_channel = static_cast<double>(w(2)) / static_cast<double>(ch);
    const double low = static_cast<double>(static_cast<s32>(static_cast<double>(w(1)) * 0.5));
    if (!(per_channel >= low) || !(per_channel <= static_cast<double>(w(1)) * 6.0)) return kAacInvalid;
    if (!(w(4) == 2 || w(4) == 0 || w(4) == 1) || !flag(5) || !flag(6) || !flag(7)) return kAacInvalid;
    if (const u32 map = r32(ctx, c + 0x20)) {
        for (s32 i = 0; i < ch; i++) {
            const s32 v = static_cast<s32>(r32(ctx, map + 4 * i));
            const u32 limits = r32(ctx, c + 0x24);
            const s32 lim = limits ? static_cast<s32>(r32(ctx, limits + 4 * i)) : ch;
            if (lim < 0 ? (v <= lim || v > 0) : (v < 0 || v >= lim)) return kAacInvalid;
        }
    }
    if (!flag(0xB) || !flag(0xF) || !flag(0x10) || !flag(0x11) || (w(0x11) == 1 && ch != 2) || !flag(0x12) ||
        static_cast<u32>(w(0x13)) >= 4 || !flag(0x14))
        return kAacInvalid;
    std::memmove(gb(ctx) + io, gb(ctx) + c, 0x80);
    const auto table = [&](u32 field, u32 at, u32 size) {
        const u32 src = r32(ctx, c + field);
        if (!src) {
            w32(ctx, io + field, 0);
            return;
        }
        std::memmove(gb(ctx) + io + at, gb(ctx) + src, size);
        w32(ctx, io + field, io + at);
    };
    table(0x20, 0x80, 0x20);
    table(0x24, 0xA0, 0x20);
    table(0x28, 0xC0, 0x100);
    return ask(ctx, e, 0x10, 0x200, 0);
}

// The sizes for a kind of input (kind 3: the input frame's bytes, 4 bytes a sample a channel of 1024, and the largest
// unit's) and the encoder's delay in samples.
s32 aac_sizes(PPUContext& ctx, u32 e, u32 kind, u32 out)
{
    if (!out) return kAacInvalid;
    u32 mult;
    switch (kind) {
    case 0: mult = 2; break;
    case 1: mult = 3; break;
    case 2:
    case 3: mult = 4; break;
    default: return kAacInvalid;
    }
    command(ctx, e, 0x12, 0, kind);
    if (push(ctx, e) != 0 || pop(ctx, e) != 0) return kAacFatal;
    const u32 v = r32(ctx, msg(ctx, e) + 8);
    w32(ctx, out, v * mult * 0x400);
    w32(ctx, out + 4, v * 0x309 + 0x7D);
    return 0;
}

s32 aac_delay(PPUContext& ctx, u32 e, u32 out)
{
    if (!out) return kAacInvalid;
    command(ctx, e, 0x13, 0, 0);
    if (push(ctx, e) != 0 || pop(ctx, e) != 0) return kAacFatal;
    w32(ctx, out, r32(ctx, msg(ctx, e) + 8));
    return static_cast<s32>(r32(ctx, msg(ctx, e) + 0xC));
}

// One input frame sent (copied to the I/O buffer, padded to 128 bytes); its unit comes later.
s32 aac_encode(PPUContext& ctx, u32 e, u32 src, s32 size, u32 flag)
{
    const u32 io = r32(ctx, e + ae::kIo);
    if (!src || size < 0) return kAacInvalid;
    if (!r8(ctx, msg(ctx, e) + 0x40)) return kAacState;
    std::memmove(gb(ctx) + io, gb(ctx) + src, static_cast<u32>(size));
    const u32 padded = align(static_cast<u32>(size), 128);
    std::memset(gb(ctx) + io + static_cast<u32>(size), 0, padded - static_cast<u32>(size));
    command(ctx, e, 0x104, padded, static_cast<u32>(size) << 8 | (flag & 0xFF));
    return push(ctx, e) != 0 ? kAacFatal : 0;
}

// The end of the input: the encoder's remaining units come later.
s32 aac_flush(PPUContext& ctx, u32 e, u32 flag)
{
    if (!r8(ctx, msg(ctx, e) + 0x40)) return kAacState;
    command(ctx, e, 0x105, 0, flag);
    return push(ctx, e) != 0 ? kAacFatal : 0;
}

// The unit of the last frame sent (or flush) taken into `dst`, its size at `size_out`.
s32 aac_unit(PPUContext& ctx, u32 e, u32 dst, u32 size_out)
{
    u32 size = 0;
    s32 r;
    const u32 m = msg(ctx, e);
    if (!dst || !size_out) {
        r = kAacInvalid;
    } else if (!r8(ctx, m + 0x40)) {
        r = kAacState;
    } else if (r32(ctx, m) == 0x104 || r32(ctx, m) == 0x105) {
        if (pop(ctx, e) != 0) {
            r = kAacFatal;
        } else {
            size = r32(ctx, m + 4);
            r = static_cast<s32>(r32(ctx, m + 0xC));
            if (r == 0 && static_cast<s32>(size) > 0) std::memmove(gb(ctx) + dst, gb(ctx) + r32(ctx, e + ae::kIo), size);
        }
    } else {
        r = kAacState;
    }
    if (size_out) w32(ctx, size_out, size);
    return r;
}

// ------------------------------------------------------------------------------------------------- the pipeline

namespace ap {
constexpr u32 kRec = 0, kSlot = 4, kParams = 8, kClock = 0xC, kHandle = 0x10, kCodec = 0x18, kInRate = 0x1C,
              kRate = 0x20, kTimescale = 0x24, kDownmix = 0x28, kStride = 0x2C, kInStride = 0x30, kBuffer = 0x34,
              kBufferSize = 0x38, kModule = 0x3C, kSrcBlock = 0x40, kSrc = 0x44, kSrcIn = 0x48, kSrcOut = 0x4C,
              kSrcStride = 0x50, kSrcInBytes = 0x54, kSrcOutBytes = 0x58, kSrcOutBuffer = 0x5C, kEncInSize = 0x60,
              kEncIn = 0x64, kAacModule = 0x68, kAacWork = 0x6C, kAacDelay = 0x70, kEncoder = 0x74,
              kEncFrameBytes = 0x78, kUnitMax = 0x7C, kUnitSamples = 0x80, kEncFrameK = 0x84, kAac = 0x88,
              kUnits = 0x17C, kUnitCount = 0x180, kRing = 0x184, kUnitSlot = 0x19C, kBlock = 0x1A0, kFrame = 0x1A4,
              kFilled = 0x1A8, kFrameRead = 0x1AC, kSrcStarted = 0x1B0, kEncFilled = 0x1B4, kSrcAvail = 0x1B8,
              kSrcRead = 0x1BC, kSrcBusy = 0x1C4, kEncBusy = 0x1C8, kEncPending = 0x1CC, kSkip = 0x1D0,
              kReady = 0x1D4, kGiven = 0x1D8, kUnitWrite = 0x1DC, kUnitFree = 0x1E0, kUnitGet = 0x1E4,
              kSamples = 0x1E8, kDelay = 0x1EC, kSilence = 0x1F0, kSessionTime = 0x1F8, kSessionScale = 0x200,
              kSessionBase = 0x208, kPosition = 0x210, kSession = 0x218, kState = 0x21C, kFlush = 0x220,
              kSize = 0x228;
// The ring of units (at kRing): where the next goes, the room after it, the bytes in use before the wrap, the room at
// the start (all ones less the room skipped at the end once wrapped), the ring's size and start.
constexpr u32 kAt = 0, kRoom = 4, kUsed = 8, kWrap = 0xC, kRingSize = 0x10, kRingStart = 0x14;
}

// The firmware's resampler (cellApostSrcMini): the work size, attributes, open, a block resampled (its message 0x15),
// the first block's output, the end's output, close.
constexpr u32 kSrcQuery = 0x24F4ECD3, kSrcAttr = 0x8A6D4227, kSrcOpen = 0x7156446D, kSrcRun = 0x35506005,
              kSrcFirst = 0x736513CA, kSrcDrain = 0xC4248C51, kSrcClose = 0xCF350A4C;
constexpr u32 kSrcModule = 0xF03B;

constexpr HostImport kImports[] = {
    {{}, "cellApostSrcMini", kSrcQuery, false},
    {{}, "cellApostSrcMini", kSrcAttr, false},
    {{}, "cellApostSrcMini", kSrcOpen, false},
    {{}, "cellApostSrcMini", kSrcRun, false},
    {{}, "cellApostSrcMini", kSrcFirst, false},
    {{}, "cellApostSrcMini", kSrcDrain, false},
    {{}, "cellApostSrcMini", kSrcClose, false},
};

s32 src_call(PPUContext& ctx, u32 nid, std::initializer_list<u64> args)
{
    return sx(firmware_call(ctx, "cellApostSrcMini", nid, args));
}

u32 g(PPUContext& ctx, u32 p, u32 off) { return r32(ctx, p + off); }
void s(PPUContext& ctx, u32 p, u32 off, u32 v) { w32(ctx, p + off, v); }
u32 state(PPUContext& ctx, u32 p) { return g(ctx, p, ap::kState); }
u32 rec_of(PPUContext& ctx, u32 p) { return g(ctx, p, ap::kRec); }

double rs(double x) { return ps_ppu_round_single(x); }
s64 fctidz(double x) { return static_cast<s64>(ps_f64_bits(ps_ppu_fctid(x, PS_FPSCR_RN_ZERO))); }

// A block of the intermediate buffer to the resampler (busy until its message).
s32 resample(PPUContext& ctx, u32 p, u32 in)
{
    s(ctx, p, ap::kSrcBusy, 1);
    return src_call(ctx, kSrcRun, {g(ctx, p, ap::kSrc), in, g(ctx, p, ap::kSrcOutBuffer), 0x15, p});
}

// The intermediate buffer resampled once full, the resampler idle and its last output taken; at the end (5, 6) a part
// is cleared and sent as it is.
void resample_check(PPUContext& ctx, u32 p)
{
    const u32 size = g(ctx, p, ap::kBufferSize), buf = g(ctx, p, ap::kBuffer);
    const bool full = size == g(ctx, p, ap::kFilled), idle = !g(ctx, p, ap::kSrcBusy), taken = !g(ctx, p, ap::kSrcAvail);
    if (state(ctx, p) - 5 > 1 || full) {
        if (full && idle && taken) resample(ctx, p, buf);
        return;
    }
    if (!idle || !taken) return;
    std::memset(gb(ctx) + buf, 0, size);
    resample(ctx, p, buf);
}

void encode(PPUContext& ctx, u32 p, u32 buf, u32 size)
{
    if (g(ctx, p, ap::kCodec) != 1) return;
    aac_encode(ctx, p + ap::kAac, buf, static_cast<s32>(size), 0);
    s(ctx, p, ap::kEncBusy, 1);
}

void encode_flush(PPUContext& ctx, u32 p)
{
    if (g(ctx, p, ap::kCodec) != 1) return;
    aac_flush(ctx, p + ap::kAac, 0);
    s(ctx, p, ap::kEncBusy, 1);
}

// A full input frame to the encoder when it is idle; at the input's end (8) with nothing left, the encoder flushed (9).
void encode_from(PPUContext& ctx, u32 p, u32 buf_off, u32 size_off, u32 filled_off)
{
    const bool idle = !g(ctx, p, ap::kEncBusy);
    const u32 size = g(ctx, p, size_off);
    const bool full = size == g(ctx, p, filled_off);
    if (full && idle) return encode(ctx, p, g(ctx, p, buf_off), size);
    if (state(ctx, p) != 8 || full || !idle) return;
    s(ctx, p, ap::kState, 9);
    encode_flush(ctx, p);
}
void encode_resampled(PPUContext& ctx, u32 p) { encode_from(ctx, p, ap::kEncIn, ap::kEncInSize, ap::kEncFilled); }
void encode_buffered(PPUContext& ctx, u32 p) { encode_from(ctx, p, ap::kBuffer, ap::kBufferSize, ap::kFilled); }

// The resampler's output into the encoder's input frame; once the resampler's end (7) has nothing left, the frame
// padded with silence (8).
void copy_resampled(PPUContext& ctx, u32 p)
{
    u32 filled = g(ctx, p, ap::kEncFilled);
    const u32 size = g(ctx, p, ap::kEncInSize), room = size - filled;
    const u32 st = state(ctx, p), avail = g(ctx, p, ap::kSrcAvail);
    if (!room) return;
    const u32 dst = g(ctx, p, ap::kEncIn) + filled;
    u32 n = 0;
    if (!avail) {
        if (st != 7) return;
    } else {
        n = avail > room ? room : avail;
        const u32 read = g(ctx, p, ap::kSrcRead);
        std::memmove(gb(ctx) + dst, gb(ctx) + g(ctx, p, ap::kSrcOutBuffer) + read, n);
        filled += n;
        s(ctx, p, ap::kSrcAvail, avail - n);
        s(ctx, p, ap::kSrcRead, read + n);
        if (avail - n || st != 7) {
            if (!(avail - n)) resample_check(ctx, p);
            s(ctx, p, ap::kEncFilled, filled);
            if (g(ctx, p, ap::kEncInSize) == filled) encode_resampled(ctx, p);
            return;
        }
    }
    const u32 rest = g(ctx, p, ap::kEncInSize) - filled;
    s(ctx, p, ap::kState, 8);
    std::memset(gb(ctx) + dst + n, 0, rest);
    filled += rest;
    s(ctx, p, ap::kEncFilled, filled);
    if (g(ctx, p, ap::kEncInSize) == filled) encode_resampled(ctx, p);
}

// 6 or 8 channels mixed down to 2 (as libsail_rec, with the PPU's rounding): each side its front channel at 0.707 and
// half the others it takes, softly limited above 0.5 (x * (x * -0.3204 + 1.1602)) and clipped to [-1, 1].
void downmix(PPUContext& ctx, u32 dst, u32 src, u32 frames, bool eight)
{
    constexpr double kFront = 0.707;
    const double k1 = static_cast<double>(std::bit_cast<float>(0xBEA4096Eu));
    const double k2 = static_cast<double>(std::bit_cast<float>(0x3F94812Eu));
    u8* b = gb(ctx);
    const auto in = [&](u32 i, u32 c) { return static_cast<double>(ps_readf32(b, src + i * (eight ? 32 : 24) + 4 * c)); };
    const auto limit = [&](double x) {
        if (x > 0.5) x = rs(x * rs(std::fma(x, k1, k2)));
        if (x > 1.0) return 1.0;
        if (x < -1.0) return -1.0;
        return x;
    };
    for (u32 i = 0; i < frames; i++) {
        double l, r;
        if (eight) {
            const double a = rs(rs(in(i, 2) + in(i, 4)) + in(i, 6)), c = rs(rs(in(i, 2) + in(i, 5)) + in(i, 7));
            l = rs(std::fma(in(i, 0), kFront, a * 0.5));
            r = rs(std::fma(in(i, 1), kFront, c * 0.5));
        } else {
            const double a = rs(in(i, 1) + in(i, 3)), c = rs(in(i, 1) + in(i, 4));
            l = rs(std::fma(in(i, 0), kFront, a * 0.5));
            r = rs(std::fma(in(i, 2), kFront, c * 0.5));
        }
        ps_writef32(b, dst + 8 * i + 4, static_cast<float>(limit(r)));
        ps_writef32(b, dst + 8 * i, static_cast<float>(limit(l)));
    }
}

// The feeder's frame into the intermediate buffer: first the silence that aligns the encoder's delay, then the frame's
// samples (copied, or mixed down); at the start of a stop (4) the frame's end pads the buffer (5 with the resampler,
// else 8); a full buffer goes on; a frame used up is given back (message 0x17).
void fill(PPUContext& ctx, u32 p)
{
    u32 filled = g(ctx, p, ap::kFilled);
    const u32 size = g(ctx, p, ap::kBufferSize);
    u32 frame = g(ctx, p, ap::kFrame), read = g(ctx, p, ap::kFrameRead);
    if (size == filled || !frame) return;
    const u32 st = state(ctx, p);
    const u32 stride = g(ctx, p, ap::kStride);
    u32 dst = filled + g(ctx, p, ap::kBuffer);
    u32 n = 0;
    bool at_end;
    u32 silence = g(ctx, p, ap::kSilence);
    for (;;) {
        if (!silence) {
            const u32 in_stride = g(ctx, p, ap::kInStride);
            const u32 fit = divu(size - filled, stride);
            n = divu(r32(ctx, frame + 4) - read, in_stride);
            if (n > fit) n = fit;
            const u32 src = read + r32(ctx, frame);
            const u32 mode = g(ctx, p, ap::kDownmix);
            if (mode == 1 || mode == 2) downmix(ctx, dst, src, n, mode == 2);
            else std::memmove(gb(ctx) + dst, gb(ctx) + src, stride * n);
            filled += stride * n;
            read += in_stride * n;
            at_end = read == r32(ctx, frame + 4);
            break;
        }
        n = divu(size - filled, stride);
        if (silence < n) n = silence;
        std::memset(gb(ctx) + dst, 0, stride * n);
        filled += stride * n;
        silence = g(ctx, p, ap::kSilence) - n;
        s(ctx, p, ap::kSilence, silence);
        if (silence) {
            at_end = read == r32(ctx, frame + 4);
            break;
        }
        dst = filled + g(ctx, p, ap::kBuffer);
    }
    if (st == 4 && at_end) {
        std::memset(gb(ctx) + dst + n * stride, 0, size - filled);
        filled = size;
        s(ctx, p, ap::kState, g(ctx, p, ap::kSrcBlock) ? 5 : 8);
    }
    s(ctx, p, ap::kFilled, filled);
    if (size == filled) {
        if (!g(ctx, p, ap::kSrcBlock)) encode_buffered(ctx, p);
        else resample_check(ctx, p);
    }
    if (at_end) {
        post(ctx, rec_of(ctx, p), 0x17, p, frame);
        frame = 0;
        read = 0;
    }
    s(ctx, p, ap::kFrameRead, read);
    s(ctx, p, ap::kFrame, frame);
}

// A frame straight to the encoder (no intermediate buffer) when it is idle; at the input's end the encoder flushed.
void feed_direct(PPUContext& ctx, u32 p)
{
    if (g(ctx, p, ap::kEncBusy)) return;
    if (const u32 frame = g(ctx, p, ap::kFrame)) return encode(ctx, p, r32(ctx, frame), r32(ctx, frame + 4));
    if (state(ctx, p) == 8) {
        s(ctx, p, ap::kState, 9);
        encode_flush(ctx, p);
    }
}

// The stop completes once no unit waits for the composer (stopped: open again, 2).
void stop_check(PPUContext& ctx, u32 p)
{
    if (g(ctx, p, ap::kReady)) return;
    s(ctx, p, ap::kState, 2);
    post_completion(ctx, rec_of(ctx, p), 0, r64(ctx, p + ap::kHandle));
}

// The next ready unit's record for the composer.
void take_unit(PPUContext& ctx, u32 p, u32 out)
{
    const u32 st = state(ctx, p), i = g(ctx, p, ap::kUnitGet), n = g(ctx, p, ap::kUnitCount);
    s(ctx, p, ap::kReady, g(ctx, p, ap::kReady) - 1);
    s(ctx, p, ap::kGiven, g(ctx, p, ap::kGiven) + 1);
    w32(ctx, out, r32(ctx, g(ctx, p, ap::kUnits) + 4 * i));
    s(ctx, p, ap::kUnitGet, (i + 1) - divu(i + 1, n) * n);
    if (st == 10) stop_check(ctx, p);
}

// The ring's bytes of a unit given back (or never given: the units of the encoder's delay).
void ring_free(PPUContext& ctx, u32 p, u32 size)
{
    const u32 r = p + ap::kRing;
    const u32 used = r32(ctx, r + ap::kUsed) - size;
    w32(ctx, r + ap::kUsed, used);
    if (static_cast<s32>(r32(ctx, r + ap::kWrap)) < 0) {
        if (used) {
            w32(ctx, r + ap::kRoom, r32(ctx, r + ap::kRoom) + size);
            return;
        }
        const u32 head = r32(ctx, r + ap::kAt) - r32(ctx, r + ap::kRingStart);
        w32(ctx, r + ap::kWrap, 0);
        w32(ctx, r + ap::kUsed, head);
        w32(ctx, r + ap::kRoom, r32(ctx, r + ap::kRingSize) - head);
        return;
    }
    if (used) {
        w32(ctx, r + ap::kWrap, size + r32(ctx, r + ap::kWrap));
        return;
    }
    w32(ctx, r + ap::kWrap, 0);
    w32(ctx, r + ap::kAt, r32(ctx, r + ap::kRingStart));
    w32(ctx, r + ap::kRoom, r32(ctx, r + ap::kRingSize));
}

// An encoded unit taken into the ring (a slot of the largest unit's size reserved, the rest given back), the next
// input sent, then the unit skipped (the encoder's delay) or recorded for the composer (its time from the samples
// encoded, message 0x18).
void collect(PPUContext& ctx, u32 p)
{
    if (!g(ctx, p, ap::kEncPending)) return;
    if (g(ctx, p, ap::kReady) + g(ctx, p, ap::kGiven) >= g(ctx, p, ap::kUnitCount)) return;
    if (g(ctx, p, ap::kCodec) != 1) return;
    const u32 r = p + ap::kRing, slot = g(ctx, p, ap::kUnitSlot);
    u32 at = r32(ctx, r + ap::kAt);
    const s32 wrap = static_cast<s32>(r32(ctx, r + ap::kWrap));
    const u32 room = r32(ctx, r + ap::kRoom);
    if (wrap < 0) {
        if (slot > room) return;
        w32(ctx, r + ap::kRoom, room - slot);
        w32(ctx, r + ap::kAt, at + slot);
    } else if (slot > room) {
        if (static_cast<s32>(slot) > wrap) return;
        at = r32(ctx, r + ap::kRingStart);
        w32(ctx, r + ap::kWrap, ~room);
        w32(ctx, r + ap::kAt, at + slot);
        w32(ctx, r + ap::kRoom, static_cast<u32>(wrap) - slot);
    } else {
        w32(ctx, r + ap::kAt, at + slot);
        w32(ctx, r + ap::kUsed, slot + r32(ctx, r + ap::kUsed));
        w32(ctx, r + ap::kRoom, room - slot);
    }
    if (!at) return;
    StackVar out(ctx, gb(ctx), 16);
    if (aac_unit(ctx, p + ap::kAac, at, out.addr()) < 0) return;
    const u32 size = r32(ctx, out.addr());
    s(ctx, p, ap::kEncBusy, 0);
    s(ctx, p, ap::kEncPending, g(ctx, p, ap::kEncPending) - 1);
    if (g(ctx, p, ap::kFlush)) {
        w32(ctx, r + ap::kAt, r32(ctx, r + ap::kRingStart));
        w32(ctx, r + ap::kRoom, r32(ctx, r + ap::kRingSize));
        w32(ctx, r + ap::kWrap, 0);
        w32(ctx, r + ap::kUsed, 0);
    } else {
        const u32 back = g(ctx, p, ap::kUnitSlot) - size;
        if (static_cast<s32>(r32(ctx, r + ap::kWrap)) >= 0) w32(ctx, r + ap::kUsed, r32(ctx, r + ap::kUsed) - back);
        w32(ctx, r + ap::kAt, r32(ctx, r + ap::kAt) - back);
        w32(ctx, r + ap::kRoom, r32(ctx, r + ap::kRoom) + back);
    }
    if (state(ctx, p) == 9) {
        if (!size) {
            ask(ctx, p + ap::kAac, 0x11, 0, 0);
            s(ctx, p, ap::kState, 10);
            stop_check(ctx, p);
            return;
        }
        encode_flush(ctx, p);
    } else if (g(ctx, p, ap::kSrcBlock)) {
        encode_resampled(ctx, p);
    } else if (g(ctx, p, ap::kBuffer)) {
        encode_buffered(ctx, p);
    } else {
        feed_direct(ctx, p);
    }
    if (g(ctx, p, ap::kFlush)) return;
    if (const u32 skip = g(ctx, p, ap::kSkip)) {
        s(ctx, p, ap::kSkip, skip - 1);
        w64(ctx, p + ap::kPosition, r64(ctx, p + ap::kPosition) + 1024);
        ring_free(ctx, p, size);
        return;
    }
    const u64 pos = r64(ctx, p + ap::kPosition);
    const double t = static_cast<double>(static_cast<s64>(pos - g(ctx, p, ap::kDelay))) /
                     static_cast<double>(static_cast<s64>(u64{g(ctx, p, ap::kRate)})) *
                     static_cast<double>(static_cast<s64>(u64{g(ctx, p, ap::kTimescale)}));
    w64(ctx, p + ap::kPosition, pos + 1024);
    const u32 i = g(ctx, p, ap::kUnitWrite), n = g(ctx, p, ap::kUnitCount);
    s(ctx, p, ap::kUnitWrite, (i + 1) - divu(i + 1, n) * n);
    const u32 unit = r32(ctx, g(ctx, p, ap::kUnits) + 4 * i);
    w32(ctx, unit + 4, size);
    w32(ctx, unit, at);
    w32(ctx, unit + 0x18, g(ctx, p, ap::kSession));
    s(ctx, p, ap::kReady, g(ctx, p, ap::kReady) + 1);
    w64(ctx, unit + 8, static_cast<u64>(fctidz(t)) + r64(ctx, p + ap::kSessionBase));
    post(ctx, rec_of(ctx, p), 0x18, p, 0);
}

// The stop's flush of the ring once the composer holds no unit: nothing ready, the ring empty.
void flush_ring(PPUContext& ctx, u32 p)
{
    if (g(ctx, p, ap::kGiven)) return;
    const u32 r = p + ap::kRing;
    s(ctx, p, ap::kReady, 0);
    w32(ctx, r + ap::kWrap, 0);
    w32(ctx, r + ap::kAt, r32(ctx, r + ap::kRingStart));
    w32(ctx, r + ap::kRoom, r32(ctx, r + ap::kRingSize));
    w32(ctx, r + ap::kUsed, 0);
    collect(ctx, p);
}

// A frame of the feeder's: a new session restarts the time base; the clock told the stream's time; then into the
// intermediate buffer (or straight to the encoder).
void frame_in(PPUContext& ctx, u32 p, u32 frame)
{
    const u32 session = r32(ctx, frame + 0xC);
    if (g(ctx, p, ap::kSession) != session) {
        s(ctx, p, ap::kSession, session);
        w64(ctx, p + ap::kPosition, 0);
        w64(ctx, p + ap::kSessionTime, r64(ctx, frame + 0x10));
        w64(ctx, p + ap::kSessionScale, r64(ctx, frame + 0x18));
        w64(ctx, p + ap::kSessionBase,
            divu64(u64{g(ctx, p, ap::kTimescale)} * r64(ctx, frame + 0x10), r64(ctx, frame + 0x18)));
    }
    if (const u32 clock = g(ctx, p, ap::kClock)) {
        const double t = static_cast<double>(static_cast<s64>(u64{g(ctx, p, ap::kSamples)})) /
                         static_cast<double>(static_cast<s64>(u64{g(ctx, p, ap::kInRate)})) *
                         static_cast<double>(static_cast<s64>(r64(ctx, p + ap::kSessionScale)));
        clock_update(ctx, clock, p, static_cast<u64>(fctidz(t)) + r64(ctx, p + ap::kSessionTime), r64(ctx, frame + 0x10),
            r64(ctx, p + ap::kSessionScale));
        s(ctx, p, ap::kSamples, g(ctx, p, ap::kSamples) + r32(ctx, frame + 8));
    }
    s(ctx, p, ap::kFrame, frame);
    s(ctx, p, ap::kFrameRead, 0);
    if (g(ctx, p, ap::kBuffer)) fill(ctx, p);
    else feed_direct(ctx, p);
}

// The resampler's block done (message 0x15): its first output, or at the end its drained output (6 more to come, 7
// none); the output into the encoder's frame.
void resampled(PPUContext& ctx, u32 p)
{
    s(ctx, p, ap::kSrcBusy, 0);
    const u32 st = state(ctx, p);
    StackVar out(ctx, gb(ctx), 16);
    if (!g(ctx, p, ap::kSrcStarted)) {
        s(ctx, p, ap::kSrcStarted, 1);
        src_call(ctx, kSrcFirst, {g(ctx, p, ap::kSrc), g(ctx, p, ap::kSrcOutBuffer), sx32(g(ctx, p, ap::kSrcIn)), out.addr()});
    } else if (st - 5 <= 1) {
        const u32 in = st == 5 ? g(ctx, p, ap::kSrcIn) : 0;
        src_call(ctx, kSrcDrain, {g(ctx, p, ap::kSrc), g(ctx, p, ap::kSrcOutBuffer), sx32(in), out.addr(), out.addr() + 4});
        s(ctx, p, ap::kState, r32(ctx, out.addr() + 4) ? 6 : 7);
    } else {
        w32(ctx, out.addr(), g(ctx, p, ap::kSrcOut));
    }
    s(ctx, p, ap::kFilled, 0);
    s(ctx, p, ap::kSrcRead, 0);
    s(ctx, p, ap::kSrcAvail, g(ctx, p, ap::kSrcStride) * r32(ctx, out.addr()));
    fill(ctx, p);
    copy_resampled(ctx, p);
}

// The encoder's unit ready (message 0x16): the input frame free again (the next one filled), the unit collected.
void encoded(PPUContext& ctx, u32 p)
{
    s(ctx, p, ap::kEncPending, g(ctx, p, ap::kEncPending) + 1);
    const bool ending = state(ctx, p) - 8 <= 1;
    if (g(ctx, p, ap::kSrcBlock)) {
        s(ctx, p, ap::kEncFilled, 0);
        if (!ending) copy_resampled(ctx, p);
    } else if (g(ctx, p, ap::kBuffer)) {
        s(ctx, p, ap::kFilled, 0);
        if (!ending) fill(ctx, p);
    } else {
        const u32 frame = g(ctx, p, ap::kFrame);
        s(ctx, p, ap::kFrame, 0);
        if (frame) post(ctx, rec_of(ctx, p), 0x17, p, frame);
    }
    collect(ctx, p);
}

// `n` units the composer gave back: their ring bytes free; on a flushing stop the ring emptied, else the next collected.
void released(PPUContext& ctx, u32 p, u32 n)
{
    s(ctx, p, ap::kGiven, g(ctx, p, ap::kGiven) - n);
    u32 i = g(ctx, p, ap::kUnitFree);
    const u32 count = g(ctx, p, ap::kUnitCount);
    for (u32 k = 0; k < n; k++) {
        const u32 unit = r32(ctx, g(ctx, p, ap::kUnits) + 4 * i);
        i = static_cast<u32>((u64{i} + 1) - divu64(u64{i} + 1, count) * count);
        ring_free(ctx, p, r32(ctx, unit + 4));
    }
    s(ctx, p, ap::kUnitFree, i);
    if (g(ctx, p, ap::kFlush)) flush_ring(ctx, p);
    else collect(ctx, p);
}

// The pipeline's resources freed (the block, the encoder and its work area, the resampler and its block and module).
s32 release(PPUContext& ctx, u32 p)
{
    const u32 rec = rec_of(ctx, p);
    if (const u32 block = g(ctx, p, ap::kBlock)) dealloc(ctx, rec, 0x80, block);
    s(ctx, p, ap::kBlock, 0);
    s(ctx, p, ap::kRing + ap::kRingStart, 0);
    s(ctx, p, ap::kBuffer, 0);
    if (g(ctx, p, ap::kCodec) == 1) {
        if (g(ctx, p, ap::kEncoder)) {
            aac_close(ctx, p + ap::kAac);
            s(ctx, p, ap::kEncoder, 0);
        }
        if (const u32 work = g(ctx, p, ap::kAacWork)) {
            dealloc(ctx, rec, 0x80, work);
            s(ctx, p, ap::kAacWork, 0);
        }
        if (g(ctx, p, ap::kAacModule)) s(ctx, p, ap::kAacModule, 0);
    }
    if (const u32 src = g(ctx, p, ap::kSrc)) {
        src_call(ctx, kSrcClose, {src});
        s(ctx, p, ap::kSrc, 0);
    }
    if (const u32 block = g(ctx, p, ap::kSrcBlock)) {
        dealloc(ctx, rec, 0x80, block);
        s(ctx, p, ap::kSrcBlock, 0);
    }
    if (const u32 m = g(ctx, p, ap::kModule)) {
        unload_module(ctx, m & 0xFFFF);
        s(ctx, p, ap::kModule, 0);
    }
    return 0;
}

// The pipeline opened for the stream's parameters (`params`, the profile's audio parameters with the format the
// feeder gives at +0x50 and the encoder's at +0x68): the encoder (its work area, libaacenc made and configured, its
// frame's and largest unit's sizes, its delay), the resampler when the slot resamples, the mix-down, the intermediate
// buffer (none when the feeder's frames are the encoder's), and one block for the buffer, the ring of units (room for
// the composer's delay at the bit rate, at least a slot for each unit the delay spans) and the units' records.
void open(PPUContext& ctx, u32 p, u32 slot, u32 params, u32 delay, u32 scale, u64 handle)
{
    const u32 rec = rec_of(ctx, p);
    s(ctx, p, ap::kTimescale, r32(ctx, params + 0x30));
    s(ctx, p, ap::kInRate, r32(ctx, params + 0x54));
    const u16 in_samples = r16(ctx, params + 0x52);
    s(ctx, p, ap::kRate, r32(ctx, params + 0x6C));
    const u32 codec = r32(ctx, params + 0x14);
    s(ctx, p, ap::kCodec, codec);
    s32 r = error(sail::CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    const auto fail = [&](s32 err) {
        release(ctx, p);
        post_completion(ctx, rec, err, handle);
    };
    if (codec != 1) return fail(r);
    s(ctx, p, ap::kAacModule, 0xF005);
    aac_work_size(ctx);
    if (const u32 work = alloc(ctx, rec, 0x80, 0x100000)) {
        s(ctx, p, ap::kAacWork, work);
        StackVar notify(ctx, gb(ctx), 16);
        w32(ctx, notify.addr(), rec + rc::kSpursQueue);
        w32(ctx, notify.addr() + 4, 0x16);
        w32(ctx, notify.addr() + 8, p);
        w32(ctx, notify.addr() + 0xC, 0);
        const u32 e = p + ap::kAac;
        if (aac_open(ctx, e, 0, work, r32(ctx, rec + rc::kSpurs), notify.addr(), slot + as::kP72, r32(ctx, slot + as::kP71)) >=
            0) {
            s(ctx, p, ap::kEncoder, e);
            StackVar sizes(ctx, gb(ctx), 16);
            if (aac_configure(ctx, e, params + 0x88) >= 0 && aac_sizes(ctx, e, 3, sizes.addr()) >= 0) {
                const u32 bytes = r32(ctx, sizes.addr());
                s(ctx, p, ap::kUnitMax, r32(ctx, sizes.addr() + 4));
                s(ctx, p, ap::kUnitSamples, 1024);
                s(ctx, p, ap::kEncFrameK, static_cast<u32>(divs(static_cast<s32>(bytes), 1024)));
                s(ctx, p, ap::kEncFrameBytes, bytes);
                aac_delay(ctx, e, p + ap::kAacDelay);
            }
        }
    }
    const bool resampling = r8(ctx, slot + as::kResample) != 0;
    u32 frame_samples = 1024, in_bytes = g(ctx, p, ap::kEncFrameBytes);
    if (resampling) {
        r = load_module(ctx, kSrcModule);
        if (r < 0) return fail(r);
        s(ctx, p, ap::kModule, kSrcModule);
        const s32 ch = static_cast<s8>(r8(ctx, params + 0x69));
        const s32 rate = static_cast<s32>(r32(ctx, params + 0x6C)), fs = static_cast<s32>(r32(ctx, params + 0x54));
        r = src_call(ctx, kSrcQuery, {sx32(static_cast<u32>(ch)), sx32(static_cast<u32>(fs)), sx32(static_cast<u32>(rate))});
        if (r < 0) return fail(r);
        // The resampler's blocks by the feeder's rate: its input samples and its output's.
        u32 in, out;
        switch (fs) {
        case 8000: in = 80, out = 480; break;
        case 11025: in = 147, out = 640; break;
        case 12000: in = 120, out = 480; break;
        case 16000: in = 160, out = 480; break;
        case 22050: in = 294, out = 640; break;
        case 24000: in = 240, out = 480; break;
        case 32000: in = 320, out = 480; break;
        case 44100: in = 588, out = 640; break;
        case 48000: in = 480, out = 480; break;
        default: return fail(error(sail::CELL_SAIL_ERROR_UNSUPPORTED_STREAM));
        }
        const u32 stride = static_cast<u32>(ch) * 4;
        const u32 enc_in = align(in_bytes, 128), work = align(static_cast<u32>(r), 128), out_bytes = out * stride;
        s(ctx, p, ap::kSrcOut, out);
        s(ctx, p, ap::kSrcInBytes, in * stride);
        s(ctx, p, ap::kSrcStride, stride);
        s(ctx, p, ap::kSrcIn, in);
        s(ctx, p, ap::kSrcOutBytes, out_bytes);
        const u32 block = alloc(ctx, rec, 0x80, (out_bytes + enc_in + work) & ~127u);
        if (!block) return fail(error(CELL_SAIL_ERROR_MEMORY));
        const u32 outbuf = block + work;
        s(ctx, p, ap::kEncIn, outbuf + out_bytes);
        s(ctx, p, ap::kSrcOutBuffer, outbuf);
        s(ctx, p, ap::kEncInSize, enc_in);
        s(ctx, p, ap::kSrcBlock, block);
        StackVar attr(ctx, gb(ctx), 0x60);
        w64(ctx, attr.addr() + 0x2C, 0x0101010101010101ull);
        w32(ctx, attr.addr() + 4, r32(ctx, rec + rc::kSpurs));
        w32(ctx, attr.addr() + 0x34, 1);
        r = src_call(ctx, kSrcAttr, {block, attr.addr()});
        if (r < 0) return fail(r);
        r = src_call(ctx, kSrcOpen, {block, sx32(static_cast<u32>(ch)), sx32(static_cast<u32>(fs)),
                                        sx32(static_cast<u32>(rate)), 16, rec + rc::kSpursQueue});
        if (r < 0) return fail(r);
        s(ctx, p, ap::kSrc, block);
        frame_samples = g(ctx, p, ap::kSrcIn);
        in_bytes = g(ctx, p, ap::kSrcInBytes);
    }
    // The mix-down: 6 or 8 channels to the encoder's 2.
    const s32 fch = static_cast<s8>(r8(ctx, params + 0x51)), ech = static_cast<s8>(r8(ctx, params + 0x69));
    u32 mode = 0;
    if (fch != ech) {
        if (ech != 2 || (fch != 6 && fch != 8)) return fail(error(sail::CELL_SAIL_ERROR_UNSUPPORTED_STREAM));
        mode = fch == 6 ? 1 : 2;
    }
    s(ctx, p, ap::kDownmix, mode);
    s(ctx, p, ap::kInStride, static_cast<u32>(fch) * 4);
    s(ctx, p, ap::kStride, static_cast<u32>(ech) * 4);
    u32 buffer = 0;
    if (!resampling && static_cast<s16>(in_samples) == static_cast<s32>(frame_samples) && fch == ech) in_bytes = 0;
    else buffer = (in_bytes + 7) & ~7u;
    s(ctx, p, ap::kBufferSize, in_bytes);
    // The ring and the units' records.
    u32 ring = divu(delay * r32(ctx, params + 0x2C), scale);
    const u32 unit_slot = align(g(ctx, p, ap::kUnitMax), 128);
    const u32 count = static_cast<u32>(
        divu64(u64{r32(ctx, params + 0x6C)} * u64{delay}, u64{scale * g(ctx, p, ap::kUnitSamples)}));
    if (ring < unit_slot * count) ring = unit_slot * count;
    const u32 table_bytes = (count * 4 + 7) & ~7u, ring_bytes = (ring + 7) & ~7u;
    const u32 block = alloc(ctx, rec, 0x80, (ring_bytes + table_bytes + buffer + count * 0x60) & ~7u);
    if (!block) return fail(error(CELL_SAIL_ERROR_MEMORY));
    s(ctx, p, ap::kBlock, block);
    s(ctx, p, ap::kBuffer, buffer ? block : 0);
    const u32 start = block + buffer, units = start + ring_bytes, rr = p + ap::kRing;
    w32(ctx, rr + ap::kAt, start);
    w32(ctx, rr + ap::kUsed, 0);
    w32(ctx, rr + ap::kRoom, ring);
    w32(ctx, rr + ap::kRingStart, start);
    w32(ctx, rr + ap::kRingSize, ring);
    w32(ctx, rr + ap::kWrap, 0);
    s(ctx, p, ap::kUnits, units);
    for (u32 i = 0; i < count; i++) w32(ctx, units + 4 * i, units + table_bytes + 0x20 * i);
    s(ctx, p, ap::kUnitSlot, unit_slot);
    s(ctx, p, ap::kUnitCount, count);
    s(ctx, p, ap::kSlot, slot);
    s(ctx, p, ap::kParams, params);
    s(ctx, p, ap::kState, 2);
    s(ctx, p, ap::kSession, 0);
    post_completion(ctx, rec, 0, handle);
}

// The units ready given to the composer as it has room.
s32 deliver(PPUContext& ctx, u32 p, u32 cs, u32 entry)
{
    u32 n = g(ctx, p, ap::kReady);
    const u32 room = r32(ctx, entry + se::kCapacity);
    if (n > room) n = room;
    StackVar unit(ctx, gb(ctx), 16);
    while (n) {
        n--;
        take_unit(ctx, p, unit.addr());
        const s32 r = composer_au(ctx, cs, entry, r32(ctx, unit.addr()), false);
        if (r < 0) return r;
    }
    return 0;
}

// The next frame taken from the feeder (its timescale set to the recorder's) into the pipeline.
s32 pump(PPUContext& ctx, u32 rec, u32 f, u32 p)
{
    StackVar out(ctx, gb(ctx), 16);
    const s32 r = feeder_take(ctx, f, out.addr(), false);
    if (r < 0) return r;
    const u32 frame = r32(ctx, out.addr());
    w64(ctx, frame + 0x18, r32(ctx, rec + rc::kTimescale));
    frame_in(ctx, p, frame);
    return 0;
}

u32 feeder_of(PPUContext& ctx, u32 p) { return r32(ctx, g(ctx, p, ap::kSlot) + as::kFeeder); }
u32 streams(PPUContext& ctx, u32 rec) { return r32(ctx, rec + rc::kStreamState); }

// The pump when the streams record (4, or 6 while Stop runs), frames wait and the pipeline has none.
void pump_if(PPUContext& ctx, u32 rec, u32 f, u32 p)
{
    const u32 st = streams(ctx, rec);
    if ((st == 4 || st == 6) && feeder_has_frames(ctx, f) && !g(ctx, p, ap::kFrame)) pump(ctx, rec, f, p);
}

} // namespace

// ------------------------------------------------------------------------------------------------- the steps

void audio_pipeline_init(PPUContext& ctx, u32 p, u32 rec, u32 clock)
{
    std::memset(gb(ctx) + p, 0, ap::kSize);
    s(ctx, p, ap::kState, 1);
    s(ctx, p, ap::kRec, rec);
    s(ctx, p, ap::kClock, clock);
}

void audio_pipeline_fini(PPUContext& ctx, u32 p)
{
    if (state(ctx, p) > 1) release(ctx, p);
    std::memset(gb(ctx) + p, 0, ap::kSize);
}

// Start (3): the frame state cleared; with an intermediate buffer, silence to align the encoder's delay to a whole
// unit (in the feeder's samples when resampling); the units of the delay to skip; the pump's message (0x14).
void audio_pipeline_start(PPUContext& ctx, u32 p, u64 handle)
{
    s(ctx, p, ap::kState, 3);
    std::memset(gb(ctx) + p + ap::kFrame, 0, 0x50);
    u32 delay = g(ctx, p, ap::kAacDelay);
    if (g(ctx, p, ap::kBuffer)) {
        const u32 pad = ((delay + 0x3FF) & ~0x3FFu) - delay;
        delay += pad;
        u32 silence = pad;
        if (g(ctx, p, ap::kSrcBlock))
            silence = static_cast<u32>(divu64(u64{pad} * g(ctx, p, ap::kSrcIn), g(ctx, p, ap::kSrcOut)));
        s(ctx, p, ap::kSilence, silence);
    }
    s(ctx, p, ap::kDelay, delay);
    s(ctx, p, ap::kSkip, delay >> 10);
    post(ctx, rec_of(ctx, p), 0x14, p, 0);
    post_completion(ctx, rec_of(ctx, p), 0, handle);
}

// Stop(flag, handle): made or open completes with 1; the flag flushes the ring; started (3) the input ends (its
// buffer padded; 8, or 5 to resample the rest); ended (10) completes when no unit waits.
void audio_pipeline_stop(PPUContext& ctx, u32 p, u32 flag, u64 handle)
{
    u32 st = state(ctx, p);
    if (st - 1 <= 1) {
        post_completion(ctx, rec_of(ctx, p), 1, handle);
        return;
    }
    s(ctx, p, ap::kFlush, flag);
    w64(ctx, p + ap::kHandle, handle);
    if (flag) {
        flush_ring(ctx, p);
        st = state(ctx, p);
    }
    if (st == 3) {
        if (!g(ctx, p, ap::kBuffer)) {
            s(ctx, p, ap::kState, 8);
            feed_direct(ctx, p);
            return;
        }
        s(ctx, p, ap::kState, 4);
        if (g(ctx, p, ap::kFrame)) return;
        const u32 filled = g(ctx, p, ap::kFilled), size = g(ctx, p, ap::kBufferSize);
        if (filled) {
            std::memset(gb(ctx) + g(ctx, p, ap::kBuffer) + filled, 0, size - filled);
            s(ctx, p, ap::kFilled, size);
        }
        if (!g(ctx, p, ap::kSrcBlock)) {
            s(ctx, p, ap::kState, 8);
            encode_buffered(ctx, p);
            return;
        }
        s(ctx, p, ap::kState, 5);
        resample_check(ctx, p);
        return;
    }
    if (st == 10) stop_check(ctx, p);
}

void audio_pipeline_close(PPUContext& ctx, u32 p, u64 handle)
{
    s32 r = 0;
    if (state(ctx, p) != 1) {
        r = release(ctx, p);
        s(ctx, p, ap::kState, 1);
    }
    post_completion(ctx, rec_of(ctx, p), r, handle);
}

// An audio stream configured once its feeder is open (libsail_rec's continuation of the feeder's open): the format
// the feeder gives (+0x50 of the stream's parameters) against the profile's (+0x38): the channels (6 or 8 mixed down
// to 2) and their layout, the sample rate (resampled to 24 or 48 kHz when the slot allows and the profile asks), the
// timescale (a multiple of the rate), the unit's duration, AAC's configuration (+0x88: its bit rate within half to six
// times the samples a second, 64000 a channel by default; the channel tables libsail_rec's); then the pipeline opened
// and the stream added to the composer (two steps for `handle`): 2, or the error.
s32 audio_stream_open(PPUContext& ctx, u32 rec, u32 record, u64 handle)
{
    const u32 slot = r32(ctx, record + 0x10);
    const u32 profile = r32(ctx, rec + rc::kProfile);
    const u32 params = r32(ctx, profile + pf::kAudio) + r8(ctx, slot + as::kIndex) * 0x108;
    const s32 r = feeder_format(ctx, r32(ctx, slot + as::kFeeder), params + 0x50);
    if (r < 0) return r;
    const s32 unsupported = error(sail::CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    std::memset(gb(ctx) + params + 0x68, 0xFF, 0x18);
    const s32 want_ch = static_cast<s8>(r8(ctx, params + 0x39));
    s32 ch = static_cast<s8>(r8(ctx, params + 0x51));
    if (want_ch != -1 && want_ch != ch) return unsupported;
    s32 layout = static_cast<s32>(r32(ctx, params + 0x58));
    const s32 want_layout = static_cast<s32>(r32(ctx, params + 0x40));
    if (static_cast<u32>(layout + 1) <= 1) {
        if (ch == 2) layout = 2;
        else if (ch == 4) layout = 6;
        else if (ch == 1) layout = 1;
    } else if (!(static_cast<u32>(layout - 1) <= 1 || layout == 12 || layout == 8 || layout == 9 || layout == 11)) {
        return unsupported;
    }
    if (want_layout != -1 && layout != want_layout) return unsupported;
    if (ch > 2) {
        if (ch != 6 && ch != 8) return unsupported;
        ch = 2;
        layout = 2;
    }
    w8(ctx, params + 0x69, static_cast<u8>(ch));
    w32(ctx, params + 0x70, static_cast<u32>(layout));
    const s32 want_rate = static_cast<s32>(r32(ctx, params + 0x3C)), fs = static_cast<s32>(r32(ctx, params + 0x54));
    s32 rate;
    const bool known = fs == 8000 || fs == 11025 || fs == 12000 || fs == 16000 || fs == 22050 || fs == 24000 ||
                       fs == 32000 || fs == 44100 || fs == 48000;
    if (r8(ctx, slot + as::kP68) && want_rate != fs && known && (want_rate == 24000 || want_rate == 48000)) {
        w8(ctx, slot + as::kResample, 1);
        rate = want_rate;
    } else {
        if (want_rate != -1 && want_rate != fs) return unsupported;
        w8(ctx, slot + as::kResample, 0);
        rate = fs;
    }
    w32(ctx, params + 0x6C, static_cast<u32>(rate));
    s32 scale = static_cast<s32>(r32(ctx, params + 0x30));
    if (scale <= 0 || scale != divs(scale, rate) * rate) scale = rate;
    const u32 codec = r32(ctx, params + 0x14);
    const s32 want_bitrate = static_cast<s32>(r32(ctx, params + 0x2C));
    w32(ctx, params + 0x30, static_cast<u32>(scale));
    w32(ctx, params + 0x80, static_cast<u32>(divs(static_cast<s32>(static_cast<u32>(scale) << 10), rate)));
    if (codec != 1) return error(sail::CELL_SAIL_ERROR_FATAL);
    w16(ctx, params + 0x6A, 0x400);
    const u32 c = params + 0x88;
    if (r32(ctx, params + 0x18)) {
        std::memset(gb(ctx) + c, 0, 0x80);
        w32(ctx, c + 0x14, codec);
        w32(ctx, c + 0x10, 0);
        w32(ctx, c + 0x1C, codec);
    }
    w32(ctx, c, static_cast<u32>(ch));
    w32(ctx, c + 4, static_cast<u32>(rate));
    // The bit rate (32-bit arithmetic, as the PPU's): 64000 a channel, or the profile's within half to six times the
    // samples a second.
    s32 bitrate = ch * 64000;
    if (want_bitrate > 0) {
        const u32 samples = static_cast<u32>(ch) * static_cast<u32>(rate);
        bitrate = static_cast<s32>(samples) >> 1;
        if (static_cast<s32>(static_cast<u32>(want_bitrate) << 1) >= static_cast<s32>(samples)) {
            const s32 most = static_cast<s32>(samples * 6);
            bitrate = want_bitrate > most ? most : want_bitrate;
        }
    }
    const u32 tables = module_copy(ctx, 0x1CC00, 0xC0);
    w32(ctx, c + 0x24, tables + 0x7C + 8 * static_cast<u32>(ch));
    w32(ctx, c + 0x20, tables + 0x6C + 8 * static_cast<u32>(ch));
    w32(ctx, c + 8, static_cast<u32>(bitrate));
    w32(ctx, c + 0x44, ch == 2 && layout == 12 ? 1 : 0);
    w32(ctx, params + 0x28, static_cast<u32>(bitrate));
    w32(ctx, params + 0x2C, static_cast<u32>(bitrate));
    open(ctx, slot + as::kPipeline, slot, params, r32(ctx, profile + pf::kDelay), r32(ctx, profile + 0x458), handle);
    composer_add(ctx, rec + rc::kComposers, slot + as::kComposerStream, params, slot + as::kPipeline, 0, handle, false);
    return 2;
}

void audio_message(PPUContext& ctx, u32 rec, u32 code, u32 a, u32 b)
{
    const u32 cs = rec + rc::kComposers;
    switch (code) {
    case 0x10: {
        // Frames out of the feeder `a`.
        const u32 p = r32(ctx, a + fd::kPipeline);
        feeder_frames_out(ctx, a, b);
        pump_if(ctx, rec, a, p);
        return;
    }
    case 0x11: {
        // The feeder's session ended (its end time): a recording slot stopped, outside Stop and the streams' stop.
        const u32 st = streams(ctx, rec);
        if (st - 5 <= 1 || st == 2) return;
        const u32 slot = r32(ctx, a + fd::kSlot);
        if (r8(ctx, slot + as::kState) != 5) return;
        const u32 r = slot + 0x30;
        w8(ctx, slot + as::kState, 6);
        std::memset(gb(ctx) + r, 0, 0x18);
        w32(ctx, r + 4, 1);
        w32(ctx, r + 0x10, slot);
        w64(ctx, r + 8, 0);
        w32(ctx, r, r32(ctx, r) | 2);
        feeder_stop(ctx, a, 0, 0, u64{r} << 32 | internal("_pspocket_cellSailRec_AudioFeederStopped"));
        return;
    }
    case 0x14: {
        // The pipeline `a` started.
        const u32 f = feeder_of(ctx, a);
        if (streams(ctx, rec) - 4 > 2 || !feeder_has_frames(ctx, f) || g(ctx, a, ap::kFrame)) return;
        pump(ctx, rec, f, a);
        return;
    }
    case 0x15: resampled(ctx, a); return;
    case 0x16: encoded(ctx, a); return;
    case 0x17: {
        // The pipeline `a` gives the feeder's frame `b` back.
        const u32 f = feeder_of(ctx, a);
        feeder_release(ctx, f, b);
        pump_if(ctx, rec, f, a);
        return;
    }
    case 0x18:
        // Units ready in the pipeline `a`.
        if (streams(ctx, rec) - 4 <= 2) deliver(ctx, a, cs, r32(ctx, a + ap::kSlot) + as::kComposerStream);
        return;
    case 0x2B: {
        // The composer gave `b` units of the stream entry `a` back.
        composer_released(ctx, cs, a, b);
        const u32 p = r32(ctx, a + se::kPipeline);
        released(ctx, p, b);
        const u32 f = feeder_of(ctx, p);
        if (streams(ctx, rec) - 4 <= 2) deliver(ctx, p, cs, a);
        pump_if(ctx, rec, f, p);
        return;
    }
    default: return;
    }
}

void register_audio(Registry& r) { r.add_imports(kChan, kImports); }

} // namespace pspocket::hle::sailrec
