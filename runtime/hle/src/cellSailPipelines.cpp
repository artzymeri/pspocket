// cellSail's per-stream pipelines (D-562 part 9, D-919), as libsail (fw 4.92) has them: what Boot sets up for each
// stream and the player's end takes down, and the decoders each opens for an elementary stream. An audio pipeline (0xD0
// bytes at the audio slot's +0x80) holds a mutex and condition pair "_saiad0" (+0x78), an lwmutex "_saiad1" (+0x80) and
// its thread "_libsail-adec_copy" (priority 450, 4 KB of stack; +0x70), which takes commands (+0x98) and counts of
// decoded frames (+0x9C) under the pair; a video pipeline (0x100 bytes at the video slot's +0x80) a pair "_saivd0"
// (+0x40), an lwmutex "_saivd1" (+0x48) and "_libsail-vdec_copy" (priority 550; +0x38), with commands at +0x60 (-1
// idle) and counts at +0x64. Command 5 (audio) and 2 (video) end the threads. Opening a stream opens its decoder
// (cellAdec, libvdec; cellVpost with video parameter 0) with the codec's parameters; closing closes them. Renderers (the
// game's, or the adapters') and the user streams' AU receivers are made up at Boot and cleaned up at the end through
// their callbacks.
//
// The copy threads' work (the decoded frames to the renderers) is a later part: given anything but their end, they
// stop the process with what was asked.

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

[[noreturn]] void apost_src(PPUContext& ctx, std::string_view call);

} // namespace sail

namespace {

using namespace sail;

// The audio pipeline (0xD0 bytes): +0 the call's handle (u64), +8 the cellAdec type, +0x10 its parameters (0x30), +0x40
// the decoder, +0x44 its module, +0x48 the converter's module, +0x4C the state (0 none, 1 closed, 2 open, 3 started),
// +0x64 the session word, +0x70 the copy thread, +0x78 the pair, +0x80 the lwmutex, +0x98 the thread's command, +0x9C
// its frame count, +0xA4 the player, +0xA8 the stream's dmux part, +0xAC the renderer, +0xB0 the slot, +0xB4 the
// decoder's memory, +0xB8 the bytes of a frame, +0xBC the converter.
namespace ap {
constexpr u32 kType = 8, kParams = 0x10, kAdec = 0x40, kAdecModule = 0x44, kPostModule = 0x48, kAlive = 0x4C,
              kSession = 0x64, kThread = 0x70, kPair = 0x78, kLwmutex = 0x80, kCommand = 0x98, kCount = 0x9C,
              kFrame = 0xA0, kPlayer = 0xA4, kEs = 0xA8, kRenderer = 0xAC, kSlot = 0xB0, kBuffer = 0xB4,
              kFrameBytes = 0xB8, kPost = 0xBC, kSize = 0xD0;
constexpr u32 kStop = 5;
} // namespace ap

// The video pipeline (0x100 bytes): +0 the call's handle, +8 the decoder, +0xC the state, +0x2C the session word, +0x30
// the decoder's memory, +0x38 the copy thread, +0x40 the pair, +0x48 the lwmutex, +0x60 the thread's command (-1 none),
// +0x64 its count, +0x70 the player, +0x74 the stream's dmux part, +0x78 the renderer, +0x7C the slot, +0x80 the
// decoder's module, +0x84 cellVpost's module, +0x88 its memory, +0x8C cellVpost, +0x90 its configuration (0x28), +0xB8
// the picture (0x20), +0xD8 the format out (0x20), +0xFC the decoder's command depth.
namespace vp {
constexpr u32 kVdec = 8, kAlive = 0xC, kSession = 0x2C, kBuffer = 0x30, kThread = 0x38, kPair = 0x40, kLwmutex = 0x48,
              kCommand = 0x60, kCount = 0x64, kPlayer = 0x70, kEs = 0x74, kRenderer = 0x78, kStream = 0x7C,
              kVdecModule = 0x80, kVpostModule = 0x84, kVpostMemory = 0x88, kVpost = 0x8C, kVpostCfg = 0x90,
              kPicture = 0xB8, kFormat = 0xD8, kDepth = 0xFC, kSize = 0x100;
constexpr u32 kIdle = 0xFFFFFFFF, kStop = 2;
} // namespace vp

u32 internal(std::string_view name) { return HleProcess::current()->link_function("cellSail", recomp::nid_of(name), name); }

// ------------------------------------------------------------------------------------------------- the copy threads

// "_libsail-adec_copy" (libsail's): under the pair it takes the command and the PCM items counted since (both cleared,
// the condition signalled), waiting while there are neither. Command 1 makes it ready (the frame's bytes from the
// pipeline, nothing held), 2 throws away the PCM it holds (each item a message 8) and gives back the frame it has
// (message 10), 3 takes the renderer's frame record (+0xA0) to fill, 4 ends the frame (given back empty, its status -1,
// when no PCM is held; else once none is, the rest of the frame zeroed), 5 ends the thread. With PCM held and a fresh
// frame it copies PCM items into the frame (cellAdecGetPcm at the frame's cursor, message 8 each) until one has data:
// the frame's status, PTS and user data from that item, and the frame goes back filled (message 10) — then it waits for
// the next command, the end's zeroing not looked at until then. With audio parameter 32 the frames go through libsail's
// sample rate converter (the firmware's libapostsrc_mini, not pspocket's).
void _pspocket_cellSail_adecCopy(PPUContext& ctx, u64 pipeline)
{
    const u32 a = static_cast<u32>(pipeline), p = r32(ctx, a + ap::kPlayer);
    u32 pending = 0, frame = 0, left = 0, cursor = 0, frame_bytes = 0;
    bool finish = false;
    StackVar item(ctx, gb(ctx), 0x40); // +0 the item's address, +8 a copy of its 0x30 bytes
    const u32 lw = a + ap::kLwmutex;
    for (;;) {
        // Wait for a command or PCM.
        const u32 mutex = r32(ctx, a + ap::kPair), cond = r32(ctx, a + ap::kPair + 4);
        sc(ctx, 102, {mutex, 0});
        u32 command = r32(ctx, a + ap::kCommand), count = r32(ctx, a + ap::kCount);
        w32(ctx, a + ap::kCommand, 0);
        pending += count;
        w32(ctx, a + ap::kCount, 0);
        sc(ctx, 108, {cond});
        while (command == 0 && count == 0) {
            sc(ctx, 107, {cond, 0});
            command = r32(ctx, a + ap::kCommand);
            count = r32(ctx, a + ap::kCount);
            w32(ctx, a + ap::kCommand, 0);
            pending += count;
            w32(ctx, a + ap::kCount, 0);
            sc(ctx, 108, {cond});
        }
        sc(ctx, 104, {mutex});
        switch (command) {
        case 1:
            frame_bytes = r32(ctx, a + ap::kFrameBytes);
            if (r8(ctx, r32(ctx, a + ap::kSlot) + as::kSrc)) apost_src(ctx, "the copy thread's conversion");
            left = 0;
            pending = 0;
            break;
        case 2:
            lv2_lib(ctx, "sys_lwmutex_lock", {lw, 0});
            for (u32 i = 0; i < pending; i++) {
                module_call(ctx, "cellAdec", "cellAdecGetPcmItem", {r32(ctx, a + ap::kAdec), item.addr()});
                module_call(ctx, "cellAdec", "cellAdecGetPcm", {r32(ctx, a + ap::kAdec), 0});
                post(ctx, p, 8, a, 0);
            }
            lv2_lib(ctx, "sys_lwmutex_unlock", {lw});
            if (frame) post(ctx, p, 10, a, frame);
            frame = 0;
            pending = 0;
            break;
        case 3:
            frame = r32(ctx, a + ap::kFrame);
            left = frame_bytes;
            cursor = r32(ctx, frame);
            break;
        case 4:
            if (pending != 0) {
                finish = true;
            } else if (frame) {
                w32(ctx, frame + 4, 0xFFFFFFFF);
                post(ctx, p, 10, a, frame);
                frame = 0;
            }
            break;
        case 5: lv2_lib(ctx, "sys_ppu_thread_exit", {0}); return;
        default: break;
        }
        // Copy PCM into a fresh frame while some is held; else zero the rest of a frame asked to end.
        for (;;) {
            if (pending != 0 && left >= frame_bytes) {
                pending--;
                lv2_lib(ctx, "sys_lwmutex_lock", {lw, 0});
                module_call(ctx, "cellAdec", "cellAdecGetPcmItem", {r32(ctx, a + ap::kAdec), item.addr()});
                std::memmove(gb(ctx) + item.addr() + 8, gb(ctx) + r32(ctx, item.addr()), 0x30);
                module_call(ctx, "cellAdec", "cellAdecGetPcm", {r32(ctx, a + ap::kAdec), cursor});
                lv2_lib(ctx, "sys_lwmutex_unlock", {lw});
                const u32 size = r32(ctx, item.addr() + 8 + 0xC);
                post(ctx, p, 8, a, 0);
                cursor += size;
                left -= size;
                if (size == 0) continue;
                w64(ctx, frame + 0x10, r64(ctx, item.addr() + 8 + 0x28));
                w32(ctx, frame + 4, r32(ctx, item.addr() + 8 + 4));
                left = 0;
                w64(ctx, frame + 8, u64{r32(ctx, item.addr() + 8 + 0x20)} << 32 | r32(ctx, item.addr() + 8 + 0x24));
                post(ctx, p, 10, a, frame);
                frame = 0;
                break;
            }
            if (finish && pending == 0) {
                std::memset(gb(ctx) + cursor, 0, left);
                cursor += left;
                finish = false;
            }
            break;
        }
    }
}

// The video copy thread's tables (libsail's data): the fields a picture structure shows (by its index, 0-8), cellVpost's
// mode and the fields past the first (by the index and whether the pictures out are interlaced), the field order
// swapped for a picture's second field, and the frame's duration in 90 kHz ticks by the frame rate code (23.976, 24,
// 25, 29.97, 30, 50, 59.94, 60 Hz; others 30).
constexpr u8 kFieldsShown[9] = {1, 1, 1, 2, 2, 3, 3, 2, 3};
constexpr u32 kVpostMode[18][2] = {{0, 0}, {0, 0}, {3, 0}, {6, 0}, {4, 0}, {7, 0}, {3, 1}, {0, 0}, {4, 1}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
constexpr u32 kSwapField[8] = {0, 2, 1, 4, 3, 5, 7, 6};
constexpr u32 kFrameTicks[9] = {3753, 3750, 3600, 3003, 3000, 1800, 1501, 1500, 3000};

// "_libsail-vdec_copy" (libsail's): under the pair it takes the command (leaving it idle, -1, the condition signalled),
// waiting while it is idle. Command 0 resets it (the next picture is the first, the format's defaults), 2 ends it, 1
// fills the renderer's frame record (+0x6C) when one is counted (+0x64): a picture's further field (+0x68 left; its PTS
// one field on, cellVpost's field order swapped), or the decoder's next picture (cellVdecGetPictureExt into the frame,
// or into cellVpost's picture first), with its status (a first picture not an I picture: -1), PTS, user data, duration
// (the frame's ticks by the rate times the fields it shows), its structure and fields; an error or a picture the
// decoder could not decode gives the frame back with the error (CELL_SAIL_ERROR_FATAL). Each picture taken is message
// 0xE {pipeline, its AUs}; the frame goes back as message 0x10 {pipeline, the fields left}.
void _pspocket_cellSail_vdecCopy(PPUContext& ctx, u64 pipeline)
{
    const u32 v = static_cast<u32>(pipeline), p = r32(ctx, v + vp::kPlayer), lw = v + vp::kLwmutex;
    // libsail's stack: the format given to the decoder {kind, colour matrix, 0, alpha (a byte), 0}, the frame record
    // staged (0x20 bytes; its last word never written), cellVpost's picture information, the picture item.
    StackVar fmt(ctx, gb(ctx), 0x20), rec(ctx, gb(ctx), 0x20), pinfo(ctx, gb(ctx), 0x40), itemp(ctx, gb(ctx), 16);
    w32(ctx, fmt.addr(), 1);
    w32(ctx, fmt.addr() + 4, 1);
    u32 ctrl = 0;
    bool first = false;
    for (;;) {
        const u32 mutex = r32(ctx, v + vp::kPair), cond = r32(ctx, v + vp::kPair + 4);
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
        if (command == 0) {
            first = true;
            ctrl = r32(ctx, v + vp::kStream) + vs::kVpostCtrl;
            w32(ctx, fmt.addr() + 4, 1);
            w32(ctx, fmt.addr() + 16, 0);
            w32(ctx, fmt.addr(), 1);
            w32(ctx, fmt.addr() + 8, 0);
            w8(ctx, fmt.addr() + 12, 0);
            continue;
        }
        if (command == vp::kStop) {
            lv2_lib(ctx, "sys_ppu_thread_exit", {0});
            return;
        }
        if (!frame) continue;
        u32 left = r32(ctx, v + 0x68), count = 0;
        const bool vpost = r8(ctx, r32(ctx, v + vp::kStream) + vs::kVpost) != 0;
        const u32 rate_index = static_cast<u32>(static_cast<s32>(static_cast<s8>(r8(ctx, v + vp::kPicture + 3)))) % 9;
        const bool interlaced = r8(ctx, v + vp::kPicture + 1) == 1;
        u32 in = 0;
        bool exec = true;
        if (left != 0) {
            // The picture's next field.
            u64 pts = r64(ctx, rec.addr() + 8);
            in = r32(ctx, v + vp::kVpostMemory);
            if (pts != ~u64{0}) {
                u64 ticks = kFrameTicks[rate_index];
                if (interlaced) ticks >>= 1;
                pts += ticks;
            }
            left--;
            w64(ctx, rec.addr() + 8, pts);
            w32(ctx, ctrl, kSwapField[r32(ctx, ctrl) & 7]);
        } else {
            lv2_lib(ctx, "sys_lwmutex_lock", {lw, 0});
            const s32 got = static_cast<s32>(module_call(ctx, "libvdec", "cellVdecGetPicItem",
                {r32(ctx, v + vp::kVdec), itemp.addr()}));
            const u32 item = r32(ctx, itemp.addr());
            if (got < 0 || r32(ctx, item + 0x44) == 1) {
                if (got < 0) {
                    w32(ctx, rec.addr() + 4, static_cast<u32>(got));
                    post(ctx, p, 0xE, v, 1);
                } else {
                    w32(ctx, fmt.addr(), 3);
                    module_call(ctx, "libvdec", "cellVdecGetPictureExt", {r32(ctx, v + vp::kVdec), fmt.addr(), 0, 0});
                    w32(ctx, rec.addr() + 4, CELL_SAIL_ERROR_FATAL);
                    post(ctx, p, 0xE, v, r8(ctx, item + 0xC));
                }
                lv2_lib(ctx, "sys_lwmutex_unlock", {lw});
                count = 0;
                exec = false;
            } else {
                // The picture's structure from the codec's information (libsail's reading of each).
                u32 not_coded_matrix = 1, range = 1, ctrl_field = 1, skip = 0, index = 0, structure = 0;
                const u32 info = r32(ctx, item + 0x48);
                const auto matrix = [&](u32 present_at, u32 coeff_at) {
                    not_coded_matrix = r8(ctx, info + present_at) == 0 || r8(ctx, info + coeff_at) - 4u > 2 ? 1 : 0;
                };
                switch (r32(ctx, item)) {
                case 0: { // MPEG-2
                    const u8 rff = r8(ctx, info + 23), tff = r8(ctx, info + 22);
                    u32 st;
                    if (r8(ctx, info + 6)) {
                        ctrl_field = 1;
                        st = rff == 0 ? 0 : tff == 0 ? 7 : 8;
                    } else {
                        const u8 ps = r8(ctx, info + 20);
                        if (ps == 1) st = 1;
                        else if (ps == 2) st = 2;
                        else if (rff != 0) st = tff == 0 ? 6 : 5;
                        else st = tff == 0 ? 4 : 3;
                        ctrl_field = r8(ctx, info + 76) == 0 ? 1 : 0;
                    }
                    matrix(9, 12);
                    structure = st;
                    skip = r8(ctx, info + 18) - 1u;
                    range = 1;
                    index = st % 9;
                    break;
                }
                case 1:
                case 7: { // AVC (and libsail's codec 7 alike)
                    const u8 ps = r8(ctx, info + 12);
                    matrix(23, 26);
                    skip = r8(ctx, info + 4);
                    range = (r8(ctx, info + 22) ^ 1) & 0xFF;
                    structure = ps;
                    ctrl_field = 1;
                    index = ps % 9;
                    break;
                }
                case 2: // MPEG-4
                    not_coded_matrix = 0;
                    skip = r8(ctx, info + 14);
                    break;
                case 5: { // VC-1
                    const u8 fcm = r8(ctx, info + 13);
                    const u32 st = fcm == 1 ? 3 : fcm == 2 ? 4 : 0;
                    matrix(9, 12);
                    skip = r8(ctx, info);
                    structure = st;
                    index = st % 9;
                    break;
                }
                default: break;
                }
                u32 shift = 0;
                if (!vpost) {
                    w32(ctx, fmt.addr(), static_cast<u32>(static_cast<s32>(static_cast<s8>(r8(ctx, v + vp::kFormat)))));
                    w8(ctx, fmt.addr() + 12, static_cast<u8>(r32(ctx, v + vp::kFormat + 0xC)));
                    const s32 cm = static_cast<s8>(r8(ctx, v + vp::kFormat + 0x10));
                    w32(ctx, fmt.addr() + 4, cm == -1 ? not_coded_matrix : static_cast<u32>(cm));
                    in = r32(ctx, r32(ctx, v + 0x6C));
                } else {
                    w32(ctx, fmt.addr(), static_cast<u32>(static_cast<s32>(static_cast<s8>(r8(ctx, v + vp::kPicture)))));
                    w8(ctx, fmt.addr() + 12, static_cast<u8>(r32(ctx, v + vp::kPicture + 0xC)));
                    const s32 cm = static_cast<s8>(r8(ctx, v + vp::kPicture + 0x10));
                    w32(ctx, fmt.addr() + 4, cm == -1 ? not_coded_matrix : static_cast<u32>(cm));
                    in = r32(ctx, v + vp::kVpostMemory);
                    const u32 mi = index * 2 + (r8(ctx, v + vp::kFormat + 1) == 1 ? 1 : 0);
                    w32(ctx, ctrl + 24, range);
                    w32(ctx, ctrl + 28, not_coded_matrix);
                    w32(ctx, ctrl + 20, ctrl_field);
                    w32(ctx, ctrl, kVpostMode[mi][0]);
                    shift = kVpostMode[mi][1];
                }
                w64(ctx, rec.addr() + 0x10, r64(ctx, item + 0x30));
                w64(ctx, rec.addr() + 8, u64{r32(ctx, item + 0x10)} << 32 | r32(ctx, item + 0x14));
                if (!first) {
                    w8(ctx, rec.addr() + 0x1A, static_cast<u8>(structure));
                    w32(ctx, rec.addr() + 4, r32(ctx, item + 0x40));
                } else if (skip != 0) {
                    w8(ctx, rec.addr() + 0x1A, static_cast<u8>(structure));
                    w32(ctx, rec.addr() + 4, 0xFFFFFFFF);
                } else {
                    w32(ctx, rec.addr() + 4, r32(ctx, item + 0x40));
                    first = false;
                    w8(ctx, rec.addr() + 0x1A, static_cast<u8>(structure));
                }
                const s32 fields = kFieldsShown[index];
                w8(ctx, rec.addr() + 0x1B, static_cast<u8>(fields));
                u32 tick = kFrameTicks[rate_index] & 0xFFFF;
                if (interlaced) tick = (tick >> 1) & 0x7FFF;
                w16(ctx, rec.addr() + 0x18, static_cast<u16>((static_cast<s64>(fields) * static_cast<s64>(tick)) >> shift));
                const u8 aus = r8(ctx, item + 0xC);
                module_call(ctx, "libvdec", "cellVdecGetPictureExt", {r32(ctx, v + vp::kVdec), fmt.addr(), in, 0});
                post(ctx, p, 0xE, v, aus);
                left = shift;
            }
        }
        if (exec) {
            // libsail unlocks the lwmutex here also for a further field, which it did not lock.
            count = left;
            lv2_lib(ctx, "sys_lwmutex_unlock", {lw});
            if (vpost) {
                module_call(ctx, "cellVpost", "cellVpostExec",
                    {r32(ctx, v + vp::kVpost), in, ctrl, r32(ctx, r32(ctx, v + 0x6C)), pinfo.addr()});
                const u32 mode = r32(ctx, ctrl);
                if (mode == 3 || mode == 4) {
                    w8(ctx, rec.addr() + 0x1A, mode == 3 ? 9 : 10);
                    w8(ctx, rec.addr() + 0x1B, 1);
                }
            }
        }
        // The frame record (its buffer kept) and the frame back.
        const u32 fr = r32(ctx, v + 0x6C);
        w32(ctx, rec.addr(), r32(ctx, fr));
        std::memmove(gb(ctx) + fr, gb(ctx) + rec.addr(), 0x20);
        post(ctx, p, 0x10, v, count);
        w32(ctx, v + 0x68, left);
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

// The decoder closed (cellAdecClose), its memory freed and its module unloaded (the fields kept when the unload fails):
// the close's answer, or the unload's error.
s32 adec_close(PPUContext& ctx, u32 a)
{
    s32 r = 0;
    if (const u32 h = r32(ctx, a + ap::kAdec)) r = static_cast<s32>(module_call(ctx, "cellAdec", "cellAdecClose", {h}));
    if (const u32 mem = r32(ctx, a + ap::kBuffer)) player_free(ctx, r32(ctx, a + ap::kPlayer), 0x80, mem);
    if (const u32 m = r32(ctx, a + ap::kAdecModule)) {
        r = unload_module(ctx, m & 0xFFFF);
        if (r < 0) return r;
    }
    w32(ctx, a + ap::kAdecModule, 0);
    w32(ctx, a + ap::kAdec, 0);
    w32(ctx, a + ap::kBuffer, 0);
    return r;
}

// libsail's sample rate converter to 48 kHz (audio parameter 32) is the firmware's libapostsrc_mini, which pspocket
// does not have: its calls stop the process with what was asked.
[[noreturn]] void apost_src(PPUContext& ctx, std::string_view call)
{
    log_call_site(ctx, gb(ctx), std::format("cellSail: {} of libapostsrc_mini (the sample rate conversion of audio "
        "parameter 32) is not pspocket's", call));
    PS_FATAL(kChan, "libsail's sample rate converter (libapostsrc_mini, {}) is not pspocket's", call);
}

// The sample rate converter closed, its memory freed, its module unloaded: 0 or the unload's error.
s32 src_close(PPUContext& ctx, u32 a)
{
    if (r32(ctx, a + ap::kPost)) apost_src(ctx, "cellApostSrcMini_CF350A4C");
    if (const u32 m = r32(ctx, a + ap::kPostModule)) {
        unload_module(ctx, m & 0xFFFF);
        w32(ctx, a + ap::kPostModule, 0);
    }
    return 0;
}

} // namespace sail

namespace {

// cellAdec's messages to an audio pipeline: an AU decoded (0) becomes message 6, PCM out (1) message 7, the sequence's
// end (3) 0x30, an error (2) message 0xD2 {error, 4, the stream's index}.
u32 _pspocket_cellSail_adecNotify(PPUContext& ctx, u32, u32 type, u32 data, u32 a)
{
    const u32 p = r32(ctx, a + ap::kPlayer);
    switch (type) {
    case 0: post(ctx, p, 6, a, 0); break;
    case 1: post(ctx, p, 7, a, 0); break;
    case 2: post_error(ctx, p, static_cast<s32>(data), 4, r8(ctx, r32(ctx, a + ap::kSlot) + as::kIndex)); break;
    case 3: post(ctx, p, 0x30, a, 0); break;
    default: break;
    }
    return 0;
}

} // namespace

namespace sail {

void audio_pipeline_fini(PPUContext& ctx, u32 a)
{
    audio_pipeline_stop(ctx, a);
    adec_close(ctx, a);
    src_close(ctx, a);
    destroy_lwmutex(ctx, a + ap::kLwmutex);
    destroy_pair(ctx, a + ap::kPair);
    w32(ctx, a + ap::kAlive, 0);
}

// Open (libsail's): with audio parameter 32 the sample rate converter first; then the codec's cellAdec parameters
// (`params`, the entry's) completed from the stream's format: LPCM's channel code by layout, AC3's layout and mixing,
// ATRAC3plus's output, AAC's rate index; its module loaded (internal ones from 0xF000), the parameters kept,
// cellAdecQueryAttr, the decoder's memory from the player's allocator, cellAdecOpenExt with the slot's resource
// (+0x15C) on the player's SPURS and libsail's callback. Open (2) with the stream, renderer and slot kept; a failure
// closes what was opened. The answer completes `handle`.
void audio_pipeline_open(PPUContext& ctx, u32 a, u32 type, u32 params, u32 format, u32 es, u32 renderer, u32 slot,
    u64 handle)
{
    const u32 p = r32(ctx, a + ap::kPlayer);
    const s32 ch = static_cast<s8>(r8(ctx, format + 1));
    const u32 ch4 = static_cast<u32>(ch * 4);
    const u32 frame = static_cast<u32>(static_cast<s16>(ps_read16(gb(ctx), format + 2))) * ch4;
    w32(ctx, a + ap::kFrameBytes, frame);
    const u32 fs = r32(ctx, format + 4);
    s32 r;
    if (r8(ctx, slot + as::kSrc)) {
        if ((r = load_module(ctx, 0xF03B)) < 0) {
            post_completion(ctx, p, r, handle);
            return;
        }
        w32(ctx, a + ap::kPostModule, 0xF03B);
        apost_src(ctx, "cellApostSrcMini_24F4ECD3");
    }
    const u32 codec = r32(ctx, type), layout = r32(ctx, format + 8);
    u32 module = 0;
    const auto unsupported = [&] {
        adec_close(ctx, a);
        post_completion(ctx, p, error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM), handle);
    };
    switch (codec) {
    case 1: { // LPCM: the channel code by layout
        u32 code;
        switch (layout) {
        case 1: code = 1; break;
        case 2: code = 3; break;
        case 8: code = 9; break;
        case 11: code = 11; break;
        default: unsupported(); return;
        }
        w32(ctx, params, code);
        module = 38;
        break;
    }
    case 2: { // AC3
        w8(ctx, params, 1);
        // Per layout: acmod, lfe, then the output channel order (5 words) and the mixing.
        static constexpr u8 kAc3[9][8] = {{0}, {1, 0, 1, 0, 2, 3, 4, 5}, {2, 0, 0, 2, 1, 3, 4, 5}, {3, 0, 0, 1, 2, 3, 4, 5},
            {4, 1, 0, 2, 1, 3, 4, 5}, {5, 1, 0, 1, 2, 3, 4, 5}, {6, 0, 0, 2, 3, 4, 5, 1}, {0}, {7, 1, 0, 1, 2, 3, 4, 5}};
        if (layout > 8 || layout == 0 || layout == 7) {
            unsupported();
            return;
        }
        const u8* v = kAc3[layout];
        w32(ctx, params + 44, v[7]);
        w8(ctx, params + 12, 2);
        w32(ctx, params + 20, 0x3F800000); // 1.0f
        w32(ctx, params + 4, 0x3F800000);
        w32(ctx, params + 8, 0x3F800000);
        w8(ctx, params + 1, v[0]);
        w8(ctx, params + 2, v[1]);
        w32(ctx, params + 24, v[2]);
        w32(ctx, params + 28, v[3]);
        w32(ctx, params + 32, v[4]);
        w32(ctx, params + 36, v[5]);
        w32(ctx, params + 40, v[6]);
        w8(ctx, params + 13, static_cast<u8>(ch));
        w8(ctx, params + 15, 0);
        w8(ctx, params + 16, 3);
        w8(ctx, params + 14, 0);
        module = 39;
        break;
    }
    case 4:
        w32(ctx, params, 4);
        module = 0xF01E;
        break;
    case 6:
        w32(ctx, params, static_cast<u32>(ch));
        w32(ctx, params + 8, 0);
        w32(ctx, params + 4, 0);
        module = 0xF01B;
        break;
    case 13: case 14: case 15: // ATRAC3plus
        w32(ctx, params + 20, 0x84);
        w32(ctx, params + 8, static_cast<u32>(ch));
        w8(ctx, params + 24, 0);
        w8(ctx, params + 25, 1);
        module = 40;
        break;
    case 16: { // the sampling rate's index
        u32 index;
        switch (fs) {
        case 96000: index = 0; break;
        case 88200: index = 1; break;
        case 64000: index = 2; break;
        case 48000: index = 3; break;
        case 44100: index = 4; break;
        case 32000: index = 5; break;
        case 24000: index = 6; break;
        case 22050: index = 7; break;
        case 16000: index = 8; break;
        case 12000: index = 9; break;
        case 11025: index = 10; break;
        case 8000: case 7350: index = 11; break;
        default: unsupported(); return;
        }
        w32(ctx, params, 2);
        w32(ctx, params + 4, index);
        w32(ctx, params + 8, 1);
        w32(ctx, params + 12, 0);
        module = 0xF01D;
        break;
    }
    default: unsupported(); return;
    }
    const auto fail = [&](s32 e) {
        adec_close(ctx, a);
        post_completion(ctx, p, e, handle);
    };
    if ((r = load_module(ctx, module & 0xF03F)) < 0) return fail(r);
    w32(ctx, a + ap::kAdecModule, module);
    std::memmove(gb(ctx) + a + ap::kParams, gb(ctx) + params, 0x30);
    StackVar attr(ctx, gb(ctx), 16);
    if ((r = static_cast<s32>(module_call(ctx, "cellAdec", "cellAdecQueryAttr", {type, attr.addr()}))) < 0) return fail(r);
    std::memmove(gb(ctx) + a + ap::kType, gb(ctx) + type, 4);
    const u32 size = r32(ctx, attr.addr());
    const u32 mem = player_alloc(ctx, p, 0x80, size);
    if (!mem) return fail(error(CELL_SAIL_ERROR_MEMORY));
    const u32 res = slot + as::kAdecResource;
    w32(ctx, res, size);
    w32(ctx, res + 16, r32(ctx, p + pl::kSpurs));
    w32(ctx, res + 4, mem);
    w32(ctx, a + ap::kBuffer, mem);
    StackVar cb(ctx, gb(ctx), 16);
    w32(ctx, cb.addr(), internal("_pspocket_cellSail_adecNotify"));
    w32(ctx, cb.addr() + 4, a);
    if ((r = static_cast<s32>(module_call(ctx, "cellAdec", "cellAdecOpenExt", {type, res, cb.addr(), a + ap::kAdec}))) < 0)
        return fail(r);
    w32(ctx, a + ap::kEs, es);
    w32(ctx, a + ap::kRenderer, renderer);
    w32(ctx, a + ap::kSession, 0);
    w32(ctx, a + ap::kSlot, slot);
    w32(ctx, a + ap::kAlive, 2);
    post_completion(ctx, p, 0, handle);
}

// Close (libsail's): the decoder and the converter closed, the pipeline closed (1); the answer (the decoder's close's,
// or the converter's error) completes `handle`.
void audio_pipeline_close(PPUContext& ctx, u32 a, u64 handle)
{
    const s32 r1 = adec_close(ctx, a);
    s32 r = src_close(ctx, a);
    if (r >= 0) r = r1 & ((r1 | (r1 - 1)) >> 31);
    w32(ctx, a + ap::kAlive, 1);
    post_completion(ctx, r32(ctx, a + ap::kPlayer), r, handle);
}


// ------------------------------------------------------------------------------------------------- audio: playing

// The pipeline's counters (libsail's): +0x50 AUs in the decoder, +0x54 PCM out not taken, +0x58 AUs the decoder takes,
// +0x5C frames the renderer may be asked for, +0x68 the sequence (1 started, 2 ended), +0x69 a start held while
// ending (2).
namespace {
constexpr u32 kInDecoder = 0x50, kPcmOut = 0x54, kTakes = 0x58, kFrames = 0x5C, kSeq = 0x68, kRestart = 0x69;
}

bool adec_takes(PPUContext& ctx, u32 a) { return r32(ctx, a + ap::kAlive) == 3 && r32(ctx, a + kTakes) != 0; }

bool adec_wants_frame(PPUContext& ctx, u32 a)
{
    return r32(ctx, a + ap::kAlive) - 3 < 2 && r32(ctx, a + kFrames) != 0;
}

// The renderer's frame record given to the copy thread (command 3).
void adec_frame(PPUContext& ctx, u32 a, u32 frame)
{
    w32(ctx, a + ap::kFrame, frame);
    w32(ctx, a + kFrames, r32(ctx, a + kFrames) - 1);
    audio_command(ctx, a, 3);
}

namespace {

// Done when nothing is in the decoder or out of it: the call completes, open (2).
void adec_idle(PPUContext& ctx, u32 a)
{
    if (r32(ctx, a + kInDecoder) != 0 || r32(ctx, a + kPcmOut) != 0) return;
    post_completion(ctx, r32(ctx, a + ap::kPlayer), 0, r64(ctx, a));
    w32(ctx, a + ap::kAlive, 2);
}

void adec_flushed(PPUContext& ctx, u32 a)
{
    if (r32(ctx, a + kFrames) != 1 || r8(ctx, a + kSeq) != 0) return;
    w32(ctx, a + ap::kAlive, 6);
    adec_idle(ctx, a);
}

// Ending (4): once the decoder is empty, the frames back and the sequence ended, the stream's end goes to the renderer
// (message 0x1B) and the call completes (2); a start held meanwhile starts again (3, the AUs it takes told: message 5).
void adec_ending(PPUContext& ctx, u32 a)
{
    if (r32(ctx, a + kInDecoder) != 0 || r32(ctx, a + kPcmOut) != 0 || r32(ctx, a + kFrames) != 1 || r8(ctx, a + kSeq))
        return;
    const u32 p = r32(ctx, a + ap::kPlayer);
    if (r8(ctx, a + kRestart) == 0) {
        post(ctx, p, 0x1B, a, r32(ctx, a + ap::kRenderer));
        post_completion(ctx, p, 0, r64(ctx, a));
        w32(ctx, a + ap::kAlive, 2);
        return;
    }
    post_completion(ctx, p, 0, r64(ctx, a));
    w8(ctx, a + kRestart, 0);
    post(ctx, p, 5, a, r32(ctx, a + kTakes));
    w32(ctx, a + ap::kAlive, 3);
}

// cellAdecEndSeq, again up to ten times 500 us apart while busy; the sequence ended (2) whatever it answers.
s32 adec_end_seq(PPUContext& ctx, u32 a)
{
    s32 r = 0;
    for (u32 tries = 10; tries; tries--) {
        r = static_cast<s32>(module_call(ctx, "cellAdec", "cellAdecEndSeq", {r32(ctx, a + ap::kAdec)}));
        if (r == 0 || static_cast<u32>(r) != 0x80610004) break;
        sc(ctx, 141, {500});
    }
    w8(ctx, a + kSeq, 2);
    return r;
}

} // namespace

// Message 0x30: the sequence ended.
void adec_seq_done(PPUContext& ctx, u32 a)
{
    w8(ctx, a + kSeq, 0);
    const u32 st = r32(ctx, a + ap::kAlive);
    if (st == 4) {
        audio_command(ctx, a, 4);
        adec_ending(ctx, a);
    } else if (st == 5) {
        adec_flushed(ctx, a);
    }
}

// Message 10: a frame filled (one more may be asked for).
void adec_frame_back(PPUContext& ctx, u32 a)
{
    w32(ctx, a + kFrames, r32(ctx, a + kFrames) + 1);
    const u32 st = r32(ctx, a + ap::kAlive);
    if (st == 4) adec_ending(ctx, a);
    else if (st == 5) adec_flushed(ctx, a);
}

// Message 8: a PCM item taken (one more AU it takes).
void adec_pcm_taken(PPUContext& ctx, u32 a)
{
    w32(ctx, a + kTakes, r32(ctx, a + kTakes) + 1);
    w32(ctx, a + kPcmOut, r32(ctx, a + kPcmOut) - 1);
    const u32 st = r32(ctx, a + ap::kAlive);
    if (st == 4) adec_ending(ctx, a);
    else if (st == 6) adec_idle(ctx, a);
}

// Message 6: an AU decoded.
void adec_au_done(PPUContext& ctx, u32 a)
{
    w32(ctx, a + kInDecoder, r32(ctx, a + kInDecoder) - 1);
    const u32 st = r32(ctx, a + ap::kAlive);
    if (st == 4) adec_ending(ctx, a);
    else if (st == 6) adec_idle(ctx, a);
}

// An AU {address, size, PTS, user data} to cellAdecDecodeAu; the session word kept (its bit 19 dropped).
s32 adec_decode(PPUContext& ctx, u32 a, u32 au)
{
    const u32 word = r32(ctx, au + 0x14) & ~0x80000u;
    if (r32(ctx, a + ap::kSession) != word) w32(ctx, a + ap::kSession, word);
    const s32 r = static_cast<s32>(module_call(ctx, "cellAdec", "cellAdecDecodeAu", {r32(ctx, a + ap::kAdec), au}));
    if (r >= 0) {
        w32(ctx, a + kInDecoder, r32(ctx, a + kInDecoder) + 1);
        w32(ctx, a + kTakes, r32(ctx, a + kTakes) - 1);
    }
    return r;
}

// The stream's end: the sequence ended, ending (4) until the decoder is empty; a failure completes the call.
void adec_end(PPUContext& ctx, u32 a, u64 handle)
{
    const s32 r = adec_end_seq(ctx, a);
    if (r >= 0) {
        w64(ctx, a, handle);
        w32(ctx, a + ap::kAlive, 4);
        return;
    }
    post_completion(ctx, r32(ctx, a + ap::kPlayer), r, handle);
}

// Stop: open already answers 1; else the sequence ended when started, the copy thread flushed (2), flushing (5) until
// the frames are back.
void adec_stop(PPUContext& ctx, u32 a, u64 handle)
{
    if (r32(ctx, a + ap::kAlive) == 2) {
        post_completion(ctx, r32(ctx, a + ap::kPlayer), 1, handle);
        return;
    }
    if (r8(ctx, a + kSeq) == 1) adec_end_seq(ctx, a);
    audio_command(ctx, a, 2);
    w64(ctx, a, handle);
    w32(ctx, a + ap::kAlive, 5);
    adec_flushed(ctx, a);
}

// Start: started answers 1; ending holds it (restart 2); else the counters reset, the copy thread made ready (1),
// cellAdecStartSeq with the parameters, started (3, the decoder taking 2 AUs: message 5); the answer completes the call.
void adec_start(PPUContext& ctx, u32 a, u64 handle)
{
    const u32 p = r32(ctx, a + ap::kPlayer), st = r32(ctx, a + ap::kAlive);
    if (st == 3) {
        post_completion(ctx, p, 1, handle);
        return;
    }
    if (st == 4) {
        w64(ctx, a, handle);
        w8(ctx, a + kRestart, 2);
        return;
    }
    w32(ctx, a + ap::kFrame, 0);
    w32(ctx, a + kTakes, 2);
    w32(ctx, a + ap::kSession, 0);
    w32(ctx, a + kInDecoder, 0);
    w32(ctx, a + kPcmOut, 0);
    w8(ctx, a + kRestart, 0);
    w32(ctx, a + ap::kCount, 0);
    w32(ctx, a + kFrames, 1);
    audio_command(ctx, a, 1);
    const s32 r = static_cast<s32>(module_call(ctx, "cellAdec", "cellAdecStartSeq", {r32(ctx, a + ap::kAdec), a + ap::kParams}));
    w8(ctx, a + kSeq, 1);
    if (r >= 0) {
        post(ctx, p, 5, a, 2);
        w32(ctx, a + ap::kAlive, 3);
    }
    post_completion(ctx, p, r, handle);
}

// Message 7: PCM out; playing or ending, counted for the copy thread; else taken and thrown away.
void adec_pcm_out(PPUContext& ctx, u32 a)
{
    if (r32(ctx, a + ap::kAlive) - 3 <= 1) {
        w32(ctx, a + kPcmOut, r32(ctx, a + kPcmOut) + 1);
        const u32 mutex = r32(ctx, a + ap::kPair), cond = r32(ctx, a + ap::kPair + 4);
        sc(ctx, 102, {mutex, 0});
        w32(ctx, a + ap::kCount, r32(ctx, a + ap::kCount) + 1);
        sc(ctx, 108, {cond});
        sc(ctx, 104, {mutex});
        return;
    }
    const u32 lw = a + ap::kLwmutex;
    lv2_lib(ctx, "sys_lwmutex_lock", {lw, 0});
    StackVar item(ctx, gb(ctx), 16);
    module_call(ctx, "cellAdec", "cellAdecGetPcmItem", {r32(ctx, a + ap::kAdec), item.addr()});
    module_call(ctx, "cellAdec", "cellAdecGetPcm", {r32(ctx, a + ap::kAdec), 0});
    lv2_lib(ctx, "sys_lwmutex_unlock", {lw});
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

// The decoder closed (cellVdecClose), its memory freed and its module unloaded (the fields kept when the unload fails).
s32 vdec_close(PPUContext& ctx, u32 v)
{
    s32 r = 0;
    if (const u32 h = r32(ctx, v + vp::kVdec)) r = static_cast<s32>(module_call(ctx, "libvdec", "cellVdecClose", {h}));
    if (const u32 mem = r32(ctx, v + vp::kBuffer)) player_free(ctx, r32(ctx, v + vp::kPlayer), 0x80, mem);
    if (const u32 m = r32(ctx, v + vp::kVdecModule)) {
        r = unload_module(ctx, m & 0xFFFF);
        if (r < 0) return r;
    }
    w32(ctx, v + vp::kVdecModule, 0);
    w32(ctx, v + vp::kVdec, 0);
    w32(ctx, v + vp::kBuffer, 0);
    return r;
}

// cellVpost closed, its memory freed and its module unloaded, when the stream's slot asks for post-processing
// (video parameter 0).
s32 vpost_close(PPUContext& ctx, u32 v)
{
    const u32 slot = r32(ctx, v + vp::kStream);
    if (!slot || !r8(ctx, slot + vs::kVpost)) return 0;
    s32 r = 0;
    if (const u32 h = r32(ctx, v + vp::kVpost)) r = static_cast<s32>(module_call(ctx, "cellVpost", "cellVpostClose", {h}));
    if (const u32 mem = r32(ctx, v + vp::kVpostMemory)) player_free(ctx, r32(ctx, v + vp::kPlayer), 0x80, mem);
    if (const u32 m = r32(ctx, v + vp::kVpostModule)) r = unload_module(ctx, m & 0xFFFF);
    w32(ctx, v + vp::kVpostModule, 0);
    w32(ctx, v + vp::kVpost, 0);
    w32(ctx, v + vp::kVpostMemory, 0);
    return r;
}

// A video pipeline's error for the player (0xD2 {error, 5, the stream's index}).
void video_error(PPUContext& ctx, u32 v, s32 err)
{
    post_error(ctx, r32(ctx, v + vp::kPlayer), err, 5, r8(ctx, r32(ctx, v + vp::kStream) + vs::kIndex));
}

// The decoder's profile or level raised to the codec's highest (libsail's fallback when cellVdec refuses one): MPEG-2
// 3, AVC level 4.2, MPEG-4 5, VC-1 14, DivX 42. 1 when it was below.
bool raise_profile(PPUContext& ctx, u32 type)
{
    const u32 codec = r32(ctx, type), level = r32(ctx, type + 4);
    u32 top;
    switch (codec) {
    case 0: top = 3; break;
    case 1: top = level > 42 ? level : 42; break; // the highest of AVC's levels (1.0 to 4.2) or the one asked
    case 2: top = 5; break;
    case 5: top = 14; break;
    case 7: top = 42; break;
    default: return false;
    }
    const bool below = (codec == 1 ? 42 : top) != level;
    if (below) w32(ctx, type + 4, top);
    return below;
}

} // namespace sail

namespace {

// cellVdec's messages to a video pipeline: an AU decoded (0) becomes message 0xC with its answer, a picture out (1)
// 0xD, the sequence's end (2) 0x32, an error (3) 0x24 with the error's low 31 bits.
u32 _pspocket_cellSail_vdecNotify(PPUContext& ctx, u32, u32 type, u32 data, u32 v)
{
    const u32 p = r32(ctx, v + vp::kPlayer);
    switch (type) {
    case 0: post(ctx, p, 0xC, v, data); break;
    case 1: post(ctx, p, 0xD, v, 0); break;
    case 2: post(ctx, p, 0x32, v, 0); break;
    case 3: post(ctx, p, 0x24, v, data & 0x7FFFFFFF); break;
    default: break;
    }
    return 0;
}

} // namespace

namespace sail {

void video_pipeline_fini(PPUContext& ctx, u32 v)
{
    video_pipeline_stop(ctx, v);
    vdec_close(ctx, v);
    vpost_close(ctx, v);
    destroy_lwmutex(ctx, v + vp::kLwmutex);
    destroy_pair(ctx, v + vp::kPair);
    w32(ctx, v + vp::kAlive, 0);
}

// cellVpost set up for the stream (video parameter 0, libsail's): the picture's size, progressive 4:2:0 in, the format
// out's kind, pitch and size; its memory (the picture's bytes and cellVpost's) from the player's allocator and the
// slot's resource (+0x21C) on the player's SPURS; then the slot's control block (+0x190) for each picture. 0 or the
// error (UNSUPPORTED for pictures or formats cellVpost does not take).
s32 vpost_open(PPUContext& ctx, u32 v, u32 picture, u32 format, u32 slot)
{
    u8* b = gb(ctx);
    const u32 cfg = v + vp::kVpostCfg, p = r32(ctx, v + vp::kPlayer);
    s32 r = load_module(ctx, 8);
    if (r < 0) return r;
    w32(ctx, v + vp::kVpostModule, 8);
    std::memset(b + cfg, 0, 0x28);
    const s32 width = static_cast<s16>(ps_read16(b, picture + 4)), height = static_cast<s16>(ps_read16(b, picture + 6));
    w32(ctx, cfg, static_cast<u32>(width));
    w32(ctx, cfg + 4, static_cast<u32>(height));
    const auto unsupported = [&] {
        vpost_close(ctx, v);
        return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    };
    if (r8(ctx, picture + 2) != 8) return unsupported();
    w32(ctx, cfg + 8, 0);
    if (r8(ctx, picture) != 3) return unsupported();
    w32(ctx, cfg + 12, 0);
    const s8 kind = static_cast<s8>(r8(ctx, format));
    const u8 depth = r8(ctx, picture + 2);
    if (kind == 2 || kind == 0) return unsupported();
    const u32 rgb = kind == 1 ? 0 : 1;
    w32(ctx, cfg + 28, rgb);
    if (kind == 1) w32(ctx, cfg + 16, static_cast<u32>(static_cast<s32>(r32(ctx, format + 8)) / 4));
    else w32(ctx, cfg + 16, r32(ctx, format + 8));
    w32(ctx, cfg + 20, static_cast<u32>(static_cast<s16>(ps_read16(b, format + 6))));
    if (r8(ctx, format + 2) != 8) return unsupported();
    w32(ctx, cfg + 24, 0);
    StackVar attr(ctx, b, 16);
    if ((r = static_cast<s32>(module_call(ctx, "cellVpost", "cellVpostQueryAttr", {cfg, attr.addr()}))) < 0) {
        vpost_close(ctx, v);
        return r;
    }
    const s32 bytes = static_cast<s8>(depth) * width * height * 3;
    const u32 picture_bytes = static_cast<u32>(bytes / 16 + 127) & ~127u;
    const u32 mem = player_alloc(ctx, p, 0x80, picture_bytes + r32(ctx, attr.addr()));
    if (!mem) {
        vpost_close(ctx, v);
        return error(CELL_SAIL_ERROR_MEMORY);
    }
    const u32 res = slot + vs::kVpostResource;
    w32(ctx, res, mem + picture_bytes);
    w32(ctx, v + vp::kVpostMemory, mem);
    w32(ctx, res + 4, r32(ctx, attr.addr()));
    w32(ctx, res + 8, r32(ctx, p + pl::kSpurs));
    if ((r = static_cast<s32>(module_call(ctx, "cellVpost", "cellVpostOpenEx", {cfg, res, v + vp::kVpost}))) < 0) {
        vpost_close(ctx, v);
        return r;
    }
    const u32 c = slot + vs::kVpostCtrl;
    const u32 mode = r8(ctx, picture + 1) == 1 ? (static_cast<s8>(r8(ctx, format + 1)) == 1 ? 5u : 3u)
                                               : (r8(ctx, format + 1) == 1 ? 1u : 0u);
    const s32 h = static_cast<s16>(ps_read16(b, picture + 6)), w = static_cast<s16>(ps_read16(b, picture + 4));
    w32(ctx, c + 40, static_cast<u32>(w));
    w32(ctx, c + 44, static_cast<u32>(h));
    w32(ctx, c + 12, static_cast<u32>(w));
    w32(ctx, c + 16, static_cast<u32>(h));
    w32(ctx, c + 32, 0);
    w32(ctx, c + 36, 0);
    w32(ctx, c, mode);
    w32(ctx, c + 24, 1);
    w32(ctx, c + 20, 1);
    u32 pitch = r32(ctx, format + 8);
    if (rgb == 0) pitch >>= 2;
    w32(ctx, c + 48, pitch);
    w32(ctx, c + 52, static_cast<u32>(static_cast<s16>(ps_read16(b, format + 6))));
    w32(ctx, c + 60, 0);
    w8(ctx, c + 72, static_cast<u8>(r32(ctx, format + 12)));
    w32(ctx, c + 68, static_cast<u32>(static_cast<s16>(ps_read16(b, format + 6))));
    w32(ctx, c + 64, static_cast<u32>(static_cast<s16>(ps_read16(b, format + 4))));
    w32(ctx, c + 56, 0);
    return 0;
}

// Open (libsail's): with video parameter 0, cellVpost first; the picture and the format kept; the codec's module
// (MPEG-2 36, AVC 37, MPEG-4 0xF021, VC-1 60, DivX 0xF036) loaded; cellVdecQueryAttrEx (the profile raised to the
// highest while refused), the decoder's memory from the player's allocator and cellVdecOpenEx with the slot's resource
// (+0x1F0: the SPUs by codec and level, the player's SPURS) and libsail's callback (refused: the profile raised, the
// memory freed and the original type queried again). Open (2) with the stream, renderer and slot kept; a failure closes
// what was opened. The answer completes `handle`.
void video_pipeline_open(PPUContext& ctx, u32 v, u32 type, u32 picture, u32 format, u32 es, u32 renderer, u32 slot,
    u64 handle)
{
    u8* b = gb(ctx);
    const u32 p = r32(ctx, v + vp::kPlayer);
    s32 r;
    if (r8(ctx, slot + vs::kVpost) && (r = vpost_open(ctx, v, picture, format, slot)) < 0) {
        post_completion(ctx, p, r, handle);
        return;
    }
    std::memmove(b + v + vp::kPicture, b + picture, 0x20);
    std::memmove(b + v + vp::kFormat, b + format, 0x20);
    const auto fail = [&](s32 e) {
        vdec_close(ctx, v);
        vpost_close(ctx, v);
        post_completion(ctx, p, e, handle);
    };
    const u32 codec = r32(ctx, type);
    u32 module = 0, spus = 0;
    switch (codec) {
    case 0: spus = r8(ctx, slot + vs::kSpus); module = 36; break;
    case 1: spus = r8(ctx, slot + (r32(ctx, type + 4) > 30 ? vs::kSpusHigh : vs::kSpus)); module = 37; break;
    case 2: spus = 1; module = 0xF021; break;
    case 5:
        spus = r32(ctx, type + 4) > 12 ? 2 : 1;
        module = 60;
        if (!r8(ctx, slot + vs::kVc1)) ps_write16(b, r32(ctx, type + 8) + 8, 3);
        break;
    case 7: spus = 1; module = 0xF036; break;
    default: return fail(error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM));
    }
    if ((r = load_module(ctx, module & 0xF03F)) < 0) return fail(r);
    w32(ctx, v + vp::kVdecModule, module);
    StackVar local(ctx, b, 0x20); // the type queried (12 bytes), then the attribute
    const u32 t = local.addr(), attr = local.addr() + 12;
    const auto original = [&] { std::memmove(b + t, b + type, 12); };
    original();
    for (;;) {
        while ((r = static_cast<s32>(module_call(ctx, "libvdec", "cellVdecQueryAttrEx", {t, attr}))) < 0)
            if (!raise_profile(ctx, t)) return fail(r);
        const u32 size = r32(ctx, attr);
        w8(ctx, v + vp::kDepth, r8(ctx, attr + 4));
        const u32 mem = player_alloc(ctx, p, 0x80, size);
        if (!mem) return fail(error(CELL_SAIL_ERROR_MEMORY));
        const u32 res = slot + vs::kVdecResource, spurs = slot + vs::kSpursResource;
        w32(ctx, res, mem);
        w32(ctx, v + vp::kBuffer, mem);
        w32(ctx, spurs, r32(ctx, p + pl::kSpurs));
        w32(ctx, spurs + 12, spus);
        w32(ctx, res + 24, spurs);
        w32(ctx, res + 4, size);
        w32(ctx, res + 20, spus);
        StackVar cb(ctx, b, 16);
        w32(ctx, cb.addr(), internal("_pspocket_cellSail_vdecNotify"));
        w32(ctx, cb.addr() + 4, v);
        r = static_cast<s32>(module_call(ctx, "libvdec", "cellVdecOpenEx", {t, res, cb.addr(), v + vp::kVdec}));
        if (r >= 0) break;
        if (!raise_profile(ctx, t)) return fail(r);
        if (const u32 m = r32(ctx, v + vp::kBuffer)) {
            player_free(ctx, p, 0x80, m);
            w32(ctx, v + vp::kBuffer, 0);
        }
        original();
    }
    w32(ctx, v + vp::kSession, 0);
    w32(ctx, v + vp::kEs, es);
    w32(ctx, v + vp::kRenderer, renderer);
    w32(ctx, v + vp::kStream, slot);
    w32(ctx, v + vp::kAlive, 2);
    post_completion(ctx, p, 0, handle);
}

// Close (libsail's): the decoder and cellVpost closed (a failure an error message), the pipeline closed (1); the
// answer completes `handle`.
void video_pipeline_close(PPUContext& ctx, u32 v, u64 handle)
{
    s32 r = vdec_close(ctx, v), result = 0;
    if (r < 0) {
        video_error(ctx, v, r);
        result = r;
    }
    r = vpost_close(ctx, v);
    if (r < 0) {
        video_error(ctx, v, r);
        result = r;
    }
    w32(ctx, v + vp::kAlive, 1);
    post_completion(ctx, r32(ctx, v + vp::kPlayer), result, handle);
}


// ------------------------------------------------------------------------------------------------- video: playing

// The video pipeline's counters (libsail's): +0x10 the decode mode, +0x14 frames to skip, +0x18 AUs in the decoder,
// +0x1C pictures out not taken, +0x20 AUs the decoder takes, +0x24 frames the renderer may be asked for, +0x28 the
// pictures it holds, +0x68 ?, +0x6C the frame record for the copy thread, +0xF8 the renderer keeps pictures, +0xFD a
// restart (1 new session, 2 start held), +0xFE the sequence started.
namespace {
constexpr u32 kVMode = 0x10, kVSkip = 0x14, kVInDecoder = 0x18, kVPicOut = 0x1C, kVTakes = 0x20, kVFrames = 0x24,
              kVHeld = 0x28, kV68 = 0x68, kVFrame = 0x6C, kVKeep = 0xF8, kVRestart = 0xFD, kVSeq = 0xFE;

// The copy thread told `command` once it has taken the last (-1); command 1 counts a frame to fill (+0x64). The
// signal's answer (the lock's error when it fails).
s32 video_command(PPUContext& ctx, u32 v, u32 command)
{
    const u32 mutex = r32(ctx, v + vp::kPair), cond = r32(ctx, v + vp::kPair + 4);
    if (const s32 r = sc(ctx, 102, {mutex, 0}); r < 0) return r;
    while (r32(ctx, v + vp::kCommand) != vp::kIdle) sc(ctx, 107, {cond, 0});
    w32(ctx, v + vp::kCommand, command);
    if (command == 1) w32(ctx, v + vp::kCount, r32(ctx, v + vp::kCount) + 1);
    const s32 r = sc(ctx, 108, {cond});
    sc(ctx, 104, {mutex});
    return r;
}

void discard_picture(PPUContext& ctx, u32 v)
{
    StackVar t(ctx, gb(ctx), 16); // +0 the item, +4 the format {1, 1, alpha 0}
    module_call(ctx, "libvdec", "cellVdecGetPicItem", {r32(ctx, v + vp::kVdec), t.addr()});
    w8(ctx, t.addr() + 12, 0);
    w32(ctx, t.addr() + 8, 1);
    w32(ctx, t.addr() + 4, 1);
    module_call(ctx, "libvdec", "cellVdecGetPicture", {r32(ctx, v + vp::kVdec), t.addr() + 4, 0});
}

// Done when nothing is in the decoder and the sequence is over: the call completes, open (2). 1 when it was.
bool vdec_idle(PPUContext& ctx, u32 v)
{
    if (r32(ctx, v + kVInDecoder) != 0 || r8(ctx, v + kVSeq)) return false;
    post_completion(ctx, r32(ctx, v + vp::kPlayer), 0, r64(ctx, v));
    w32(ctx, v + vp::kAlive, 2);
    return true;
}

// Flushing (5): once the renderer has its frames back, the pictures out thrown away, 6, then idle.
bool vdec_flushed(PPUContext& ctx, u32 v)
{
    if (r32(ctx, v + kVFrames) != 1) return false;
    const u32 lw = v + vp::kLwmutex;
    lv2_lib(ctx, "sys_lwmutex_lock", {lw, 0});
    for (s32 n = static_cast<s32>(r32(ctx, v + kVPicOut)); n > 0; n--) discard_picture(ctx, v);
    w32(ctx, v + kV68, 0);
    w32(ctx, v + kVPicOut, 0);
    lv2_lib(ctx, "sys_lwmutex_unlock", {lw});
    w32(ctx, v + vp::kAlive, 6);
    vdec_idle(ctx, v);
    return true;
}

// cellVdecStartSeq (VC-1 with its frame rate code below 8 also cellVdecSetFrameRate 0x80 + the code), the sequence
// started, the copy thread reset (0).
s32 vdec_start_seq(PPUContext& ctx, u32 v)
{
    module_call(ctx, "libvdec", "cellVdecStartSeq", {r32(ctx, v + vp::kVdec)});
    const u8 rate = r8(ctx, v + vp::kPicture + 3);
    if (r32(ctx, v + vp::kVdecModule) == 0x3C && rate < 8) {
        const s32 r = static_cast<s32>(module_call(ctx, "libvdec", "cellVdecSetFrameRate",
            {r32(ctx, v + vp::kVdec), u32{0x80} + static_cast<u32>(static_cast<s8>(rate))}));
        if (r < 0) return r;
    }
    w8(ctx, v + kVSeq, 1);
    return video_command(ctx, v, 0);
}

// Ending (4): once the decoder, the pictures and the frames are all in and the sequence is over, the stream's end goes
// to the renderer (0x1C) and the call completes (2); a new session or held start starts the sequence again (3).
bool vdec_ending(PPUContext& ctx, u32 v)
{
    if (r32(ctx, v + kVInDecoder) != 0 || r32(ctx, v + kVPicOut) != 0 || r32(ctx, v + kVKeep) != 0 ||
        r32(ctx, v + kVFrames) != 1 || r8(ctx, v + kVSeq))
        return false;
    const u32 p = r32(ctx, v + vp::kPlayer);
    if (r8(ctx, v + kVRestart) == 0) {
        post(ctx, p, 0x1C, v, r32(ctx, v + vp::kRenderer));
        post_completion(ctx, p, 0, r64(ctx, v));
        w32(ctx, v + vp::kAlive, 2);
        return true;
    }
    vdec_start_seq(ctx, v);
    if (r64(ctx, v) != 0) post_completion(ctx, p, 0, r64(ctx, v));
    w8(ctx, v + kVRestart, 0);
    post(ctx, p, 0xB, v, r32(ctx, v + kVTakes));
    w32(ctx, v + vp::kAlive, 3);
    return true;
}

// cellVdecEndSeq, again up to ten times 500 us apart while busy; the sequence ended (2) whatever it answers.
s32 vdec_end_seq(PPUContext& ctx, u32 v)
{
    s32 r = 0;
    for (u32 tries = 10; tries; tries--) {
        r = static_cast<s32>(module_call(ctx, "libvdec", "cellVdecEndSeq", {r32(ctx, v + vp::kVdec)}));
        if (r == 0 || static_cast<u32>(r) != 0x80610103) break;
        sc(ctx, 141, {500});
    }
    w8(ctx, v + kVSeq, 2);
    return r;
}

} // namespace

bool vdec_takes(PPUContext& ctx, u32 v)
{
    return r32(ctx, v + vp::kAlive) == 3 && static_cast<s32>(r32(ctx, v + kVTakes)) > 0;
}

void vdec_frame_wanted(PPUContext& ctx, u32 v) { w32(ctx, v + kVFrames, r32(ctx, v + kVFrames) + 1); }

bool vdec_has_picture(PPUContext& ctx, u32 v)
{
    if (r32(ctx, v + kVFrames) == 0) return false;
    if (r32(ctx, v + kVPicOut) != 0) return true;
    return r32(ctx, v + kVKeep) != 0;
}

// The renderer's frame record to fill (command 1).
void vdec_frame(PPUContext& ctx, u32 v, u32 frame)
{
    w32(ctx, v + kVFrames, r32(ctx, v + kVFrames) - 1);
    if (r32(ctx, v + kVKeep) == 0) w32(ctx, v + kVPicOut, r32(ctx, v + kVPicOut) - 1);
    w32(ctx, v + kVFrame, frame);
    video_command(ctx, v, 1);
}

// Message 0xD: a picture out; flushing or done, thrown away.
void vdec_picture_out(PPUContext& ctx, u32 v)
{
    if (r32(ctx, v + vp::kAlive) - 5 > 1) {
        w32(ctx, v + kVPicOut, r32(ctx, v + kVPicOut) + 1);
        return;
    }
    const u32 lw = v + vp::kLwmutex;
    lv2_lib(ctx, "sys_lwmutex_lock", {lw, 0});
    discard_picture(ctx, v);
    lv2_lib(ctx, "sys_lwmutex_unlock", {lw});
}

// Message 0x10: a frame filled; the renderer keeps pictures from now when told so; one more frame may be asked for.
void vdec_frame_back(PPUContext& ctx, u32 v, u32 keep, u32 out)
{
    w32(ctx, v + kVKeep, keep);
    w32(ctx, out, r32(ctx, v + kVFrame));
    w32(ctx, v + kVFrames, r32(ctx, v + kVFrames) + 1);
    const u32 st = r32(ctx, v + vp::kAlive);
    if (st == 5) vdec_flushed(ctx, v);
    else if (st == 4) vdec_ending(ctx, v);
}

// Message 0x32: the sequence ended.
void vdec_seq_done(PPUContext& ctx, u32 v)
{
    w8(ctx, v + kVSeq, 0);
    const u32 st = r32(ctx, v + vp::kAlive);
    if (st == 4) vdec_ending(ctx, v);
    else if (st == 6) vdec_idle(ctx, v);
}

// Message 0xC: an AU decoded (its answer -1: none decoded).
void vdec_au_done(PPUContext& ctx, u32 v, s32 result)
{
    if (result == -1) return;
    w32(ctx, v + kVTakes, r32(ctx, v + kVTakes) + 1);
    w32(ctx, v + kVInDecoder, r32(ctx, v + kVInDecoder) - 1);
    const u32 st = r32(ctx, v + vp::kAlive);
    if (st == 4) vdec_ending(ctx, v);
    else if (st == 6) vdec_idle(ctx, v);
}

// An AU {address, size, PTS, DTS, user data} to cellVdecDecodeAu (in the pipeline's mode, or 1 while frames are to be
// skipped); a new session word with the sequence started ends it first (restart 1) and answers 1. A busy decoder
// answers 1 (the AU kept); 0 when it took it.
s32 vdec_decode(PPUContext& ctx, u32 v, u32 au)
{
    const u32 word = r32(ctx, au + 0x1C) & ~0x80000u, old = r32(ctx, v + vp::kSession);
    if (word != old) {
        w32(ctx, v + vp::kSession, word);
        if (old != 0 && r8(ctx, v + kVSeq)) {
            w8(ctx, v + kVRestart, 1);
            vdec_end(ctx, v, 0);
            return 1;
        }
    }
    u32 mode = r32(ctx, v + kVMode);
    if (static_cast<s32>(r32(ctx, v + kVSkip)) >= 1) mode = 1;
    w32(ctx, v + kVSkip, 0);
    s32 r = static_cast<s32>(module_call(ctx, "libvdec", "cellVdecDecodeAu", {r32(ctx, v + vp::kVdec), mode, au}));
    if (r < 0) {
        if (static_cast<u32>(r) == 0x80610103) r = 1;
        return r;
    }
    w32(ctx, v + kVInDecoder, r32(ctx, v + kVInDecoder) + 1);
    w32(ctx, v + kVTakes, r32(ctx, v + kVTakes) - 1);
    return 0;
}

// The stream's end: the sequence ended, ending (4); a failure completes the call.
void vdec_end(PPUContext& ctx, u32 v, u64 handle)
{
    const s32 r = vdec_end_seq(ctx, v);
    if (r >= 0) {
        w64(ctx, v, handle);
        w32(ctx, v + vp::kAlive, 4);
        return;
    }
    post_completion(ctx, r32(ctx, v + vp::kPlayer), r, handle);
}

// Stop: open answers 1; else the sequence ended when started (a failure an error message), flushing (5) with the
// pictures it holds counted as frames back.
void vdec_stop(PPUContext& ctx, u32 v, u64 handle)
{
    if (r32(ctx, v + vp::kAlive) == 2) {
        post_completion(ctx, r32(ctx, v + vp::kPlayer), 1, handle);
        return;
    }
    if (r8(ctx, v + kVSeq) == 1)
        if (const s32 r = vdec_end_seq(ctx, v); r < 0) video_error(ctx, v, r);
    const u32 held = r32(ctx, v + kVHeld);
    w32(ctx, v + kVHeld, 0);
    w32(ctx, v + vp::kAlive, 5);
    w64(ctx, v, handle);
    w32(ctx, v + kVFrames, r32(ctx, v + kVFrames) + held);
    vdec_flushed(ctx, v);
}

// Start: started answers 1; ending holds it (restart 2); else the counters reset, the frame rate code kept, the
// sequence started (3, the decoder's command depth told: message 0xB); the answer completes the call.
void vdec_start(PPUContext& ctx, u32 v, u8 rate, u64 handle)
{
    const u32 p = r32(ctx, v + vp::kPlayer), st = r32(ctx, v + vp::kAlive);
    if (st == 3) {
        post_completion(ctx, p, 1, handle);
        return;
    }
    if (st == 4) {
        w64(ctx, v, handle);
        w8(ctx, v + kVRestart, 2);
        return;
    }
    w32(ctx, v + vp::kCount, 0);
    w32(ctx, v + kVFrames, 1);
    w32(ctx, v + vp::kSession, 0);
    w8(ctx, v + vp::kPicture + 3, rate);
    w32(ctx, v + kVInDecoder, 0);
    w32(ctx, v + kVTakes, r8(ctx, v + vp::kDepth));
    w32(ctx, v + kVPicOut, 0);
    w32(ctx, v + kVHeld, 0);
    w8(ctx, v + kVRestart, 0);
    w32(ctx, v + kV68, 0);
    w32(ctx, v + kVKeep, 0);
    w32(ctx, v + kVFrame, 0);
    w32(ctx, v + kVSkip, 0);
    const s32 r = vdec_start_seq(ctx, v);
    if (r >= 0) {
        post(ctx, p, 0xB, v, r8(ctx, v + vp::kDepth));
        w32(ctx, v + vp::kAlive, 3);
    }
    post_completion(ctx, p, r, handle);
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
        ::pspocket::hle::Function{{}, "_pspocket_cellSail_adecNotify", recomp::nid_of("_pspocket_cellSail_adecNotify"),
            &detail::thunk<&_pspocket_cellSail_adecNotify>, true},
        ::pspocket::hle::Function{{}, "_pspocket_cellSail_vdecNotify", recomp::nid_of("_pspocket_cellSail_vdecNotify"),
            &detail::thunk<&_pspocket_cellSail_vdecNotify>, true},
    });
}

} // namespace pspocket::hle
