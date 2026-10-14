// libavcdec and libmvcdec, the H.264 and MVC codec cores Sony's libvdec drives (and a game may call directly),
// pspocket's own (D-933): their functions answering as Sony's modules (firmware 4.92) do, their PPU side reproduced
// (the checks, the context at the end of the decoder's memory, the command queues' room, the side data and frame
// tables, the helper thread and the messages it gives the caller's callback: one program with each module's numbers,
// Core), and what their SPU programs do with an access unit done on the host: the stream's syntax read as Sony's
// decoders report it (vdec_h264.hpp), the samples decoded by FFmpeg into the frame buffer Sony's decoders take (an MVC
// stream's second view through vdec_mvc_view.cpp), and the pictures output when each decoded picture buffer outputs
// them (libavcdec's and libmvcdec's SPU programs keep theirs differently). Compared call for call with Sony's modules
// in tests/hle/test_vdec_avc_compare.cpp.
//
// The API (the names RPCS3 has, rpcs3/Emu/Cell/Modules/libavcdec.cpp and libmvcdec.cpp; their arguments read in
// Sony's modules):
// - avcDecQueryMemory(param {level, deblocking, buffers, -, width and height in macroblocks}, attr {-, size, align})
// - avcDecOpen(&handle, spurs, &taskset, param, mem {addr, size, align}, stack, sdk, callback, arg, spus, priorities,
//   spu priority, ppu priority)
// - avcDecAccessUnit(handle, addr, size, cont addr, cont size, dts, pts, mode, side data, side size)
// - avcDecRlsFrame(handle, frame id), avcDecFlush(handle, 0), avcDecReset(handle, keep sps, flags),
//   avcDecGetStatus(handle, status[2]), avcDecClose(handle); avcDecGetVersion and avcDecQueryCharacteristics stay in
//   codec_cores.cpp (D-575).
// - mvcDecQueryMemory, mvcDecOpen (libavcdec's without the SDK version), mvcDecAccessUnit(handle, pieces {base view,
//   size, its continuation, size, second view, size, its continuation, size}, dts, pts, mode, side data, side size),
//   mvcDecRlsFrame, mvcDecFlush, mvcDecReset, mvcDecClose (no GetStatus); mvcDecGetVersion and
//   mvcDecQueryCharacteristics stay in codec_cores.cpp.
// The callback gets one argument, a message on the helper thread's stack: {type, arg, ...}: 0 an access unit done,
// 1 a picture (its information at +8), 2 the flush done, 3 the reset done, 4 a severe error.
//
// Nothing here is Sony's code or data (CLAUDE.md rule 1): the behaviour is written from what the module does.

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/object.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "vdec_avc.hpp"
#include "vdec_h264.hpp"
#include "vdec_mvc_view.hpp"

#if PSPOCKET_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}
#endif

namespace pspocket::hle::vdec::avc {

using namespace h264;

// =================================================================================================== sizes

namespace {

// libavcdec's level table (H.264 Table A-1's limits as the module keeps them: macroblocks per second, the largest frame
// in macroblocks, the decoded picture buffer in 1024-byte units x10, the minimum compression ratio), the worst-case
// picture its sizes are for without the caller's size, and the bytes it adds for the level (cellVdec.cpp's avc_memory
// is the same reckoning, D-195).
struct Level {
    u32 level, mbps, max_fs, dpb_x10, min_cr, width, height, extra;
};
constexpr std::array<Level, 13> kLevels{{
    {10, 1485, 99, 1485, 2, 80, 304, 256},
    {11, 3000, 396, 3375, 2, 80, 960, 128},
    {12, 6000, 396, 8910, 2, 80, 1072, 256},
    {13, 11880, 396, 8910, 2, 80, 1072, 128},
    {20, 11880, 396, 8910, 2, 80, 1072, 128},
    {21, 19800, 792, 17820, 2, 80, 1072, 128},
    {22, 20250, 1620, 30375, 2, 128, 1072, 128},
    {30, 40500, 1620, 30375, 2, 128, 1072, 0},
    {31, 108000, 3600, 67500, 4, 928, 992, 256},
    {32, 216000, 5120, 76800, 4, 1280, 1024, 256},
    {40, 245760, 8192, 122880, 4, 1920, 1088, 256},
    {41, 245760, 8192, 122880, 2, 1920, 1088, 256},
    {42, 245760, 8192, 122880, 2, 1920, 1088, 256},
}};

const Level* level_of(u32 level)
{
    const auto it = std::find_if(kLevels.begin(), kLevels.end(), [&](const Level& l) { return l.level == level; });
    return it == kLevels.end() ? nullptr : &*it;
}

} // namespace

std::optional<Layout> layout(u32 level, u32 buffers, u32 width_mbs, u32 height_mbs)
{
    const Level* lv = level_of(level);
    if (!lv) return std::nullopt;
    u32 width = lv->width, height = lv->height, frame_mbs = lv->max_fs;
    if (width_mbs && height_mbs) {
        if (width_mbs > 120 || width_mbs < 4 || height_mbs > 68 || height_mbs < 4) return std::nullopt;
        frame_mbs = width_mbs * height_mbs;
        if (lv->max_fs < frame_mbs) return std::nullopt;
        width = width_mbs * 16;
        height = height_mbs * 16;
    }
    u32 dpb = std::min<u32>(static_cast<u32>(((u64{lv->dpb_x10 & 0x3FFFFF} << 10) / 10 << 1) / (width * height * 3)), 16);
    if (buffers) {
        if (buffers > dpb) return std::nullopt;
        dpb = buffers;
    }
    const u32 min_cr = level > 40 ? 4 : lv->min_cr;
    u32 au = static_cast<u32>(u64{lv->mbps} * 384384 / (u64{min_cr} * 24000));
    if (au & 127) au = (au & ~127u) + 128;
    Layout l;
    l.blocks = {0x9B800, frame_mbs * 0x600, frame_mbs * (dpb + 2) * 256, (au + 0x5C00) * 3 + 256,
        ((width + 64) * height + ((width + 64) * height >> 1) + 0x800) * (dpb + 2), 0x300, 0x1900, 0x1E0000, 0x1900,
        0xAC800, 0x1980, 0x20000, 0x100, 0x200};
    if ((l.blocks[3] | l.blocks[4]) & 127) return std::nullopt;
    u32 total = 0;
    for (const u32 b : l.blocks) total += b;
    l.dynamic = total + lv->extra;
    l.size = l.dynamic + 0x299500;
    if (l.size % 128) l.size += 128 - l.size % 128;
    l.frames = l.blocks[0] + l.blocks[1] + l.blocks[2] + l.blocks[3];
    l.au_buffer = au + 0x2000;
    l.frame_mbs = frame_mbs;
    return l;
}

std::optional<Layout> mvc_layout(u32 level, u32 buffers, u32 width_mbs, u32 height_mbs)
{
    const Level* lv = level_of(level);
    if (!lv) return std::nullopt;
    u32 width = lv->width, height = lv->height, frame_mbs = lv->max_fs;
    if (width_mbs && height_mbs) {
        if (width_mbs > 120 || width_mbs < 4 || height_mbs > 68 || height_mbs < 4) return std::nullopt;
        frame_mbs = width_mbs * height_mbs;
        if (lv->max_fs < frame_mbs) return std::nullopt;
        width = width_mbs * 16;
        height = height_mbs * 16;
    }
    // Two views' buffers: twice the level's, made even, at most 32; the caller's count even and no more.
    u32 dpb = static_cast<u32>((((u64{lv->dpb_x10 & 0x3FFFFF} << 10) / 10) << 2) / (width * height * 3));
    if (dpb & 1) dpb++;
    dpb = std::min<u32>(dpb, 32);
    if (buffers) {
        if (buffers > dpb || (buffers & 1)) return std::nullopt;
        dpb = buffers;
    }
    const u32 min_cr = level > 40 ? 4 : lv->min_cr;
    const u32 au = ((((static_cast<u32>(u64{lv->mbps} * 384384 / (u64{min_cr} * 24000)) + 0x2000) * 2) + 0x7F) & ~0x7Fu) + 0x4400;
    Layout l;
    l.blocks = {0xE9400, frame_mbs * 0x600, frame_mbs * 0x200, au * 2,
        ((width + 64) * height + ((width + 64) * height >> 1) + 0x800) * (dpb + 4), 0x480, 0x4380, 0x1B7E00, 0x2700,
        0x9F600, 0x1F80, 0x280, 0x40000, 0x100, 0x300, ((dpb + 3) * frame_mbs * 0x50 + 0x7F) & ~0x7Fu};
    if (l.blocks[4] & 127) return std::nullopt;
    l.dynamic = l.blocks[1] + l.blocks[2] + l.blocks[3] + l.blocks[4] + l.blocks[15] + 0x3BD400;
    l.size = l.dynamic + 0x4FC100;
    if (l.size % 128) l.size += 128 - l.size % 128;
    l.frames = l.blocks[0] + l.blocks[1] + l.blocks[2] + l.blocks[3];
    l.au_buffer = au;
    l.frame_mbs = frame_mbs;
    return l;
}

// =================================================================================================== the frame buffer

// Sony's H.264 frame buffer (D-196): 16-byte strips of luma, two of repeated edge on each side, a gap of 0x400 bytes,
// the chroma (8 Cb then 8 Cr a line) in strips likewise, a gap; the decoder leaves the gaps as they were (D-912). A
// slot holds (width + 64) x height x 1.5 + 2048 bytes.
u32 frame_slot_bytes(u32 width, u32 height) { return (width + 64) * height + (width + 64) * height / 2 + 2048; }

void write_frame(u8* base, u32 at, u32 width, u32 height, const u8* y, const u8* u, const u8* v)
{
    const u32 strips = (width + 15) / 16 + 4, cw = width / 2, ch = height / 2;
    for (u32 s = 0; s < strips; s++)
        for (u32 row = 0; row < height; row++)
            for (u32 k = 0; k < 16; k++) {
                const s32 x = std::clamp<s32>(static_cast<s32>(s * 16 + k) - 32, 0, static_cast<s32>(width) - 1);
                base[at + s * 16 * height + row * 16 + k] = y[row * width + static_cast<u32>(x)];
            }
    const u32 chroma = at + strips * 16 * height + 0x400;
    for (u32 s = 0; s < strips; s++)
        for (u32 row = 0; row < ch; row++)
            for (u32 k = 0; k < 8; k++) {
                const s32 x = std::clamp<s32>(static_cast<s32>(s * 8 + k) - 16, 0, static_cast<s32>(cw) - 1);
                base[chroma + s * 16 * ch + row * 16 + k] = u[row * cw + static_cast<u32>(x)];
                base[chroma + s * 16 * ch + row * 16 + 8 + k] = v[row * cw + static_cast<u32>(x)];
            }
}

namespace {

constexpr std::string_view kChan = "libavcdec";
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// What tells libavcdec's PPU side from libmvcdec's: the same program with other numbers (read in Sony's modules).
struct Core {
    std::string_view library; // the module's name (and the log's channel)
    const char* prefix;       // its own SPURS instance's name prefix; its task set "<prefix>_<handle>", its thread's name
    u32 err;                  // its error codes: ARG +1, SEQ +2 (not in a state for the call), BUSY +3 (a queue or a
                              // table full), FATAL +0x81
    u32 magic;                // the open mark at +0 of the context
    // The context: the last bytes of the decoder's memory, the handle. What a caller may see of it: the open mark, the
    // side data each access unit carried (its table's entries: the size, then the data; libmvcdec's then count the
    // releases). The rest is Sony's PPU side's bookkeeping, kept on the host here.
    u32 context_bytes;
    u32 side_at, side_entries, side_entry;
    u32 frame_entries;          // the frame table (what a release frees)
    u32 au_room, other_room;    // the queues' entries: a queue answers BUSY with fewer than two free
    // The fixed part of the memory before the SPU side's working memory (task contexts, queues; Open clears it up to
    // the core's own SPURS), its own SPURS, the task set.
    u32 spurs_at, taskset_at, dynamic_at;
    u32 extra;                  // QueryMemory: the size past the dynamic part
    u32 helper_nid;             // the helper thread's entry (pspocket's own function)
    const char* helper;
    bool mvc;

    s32 arg() const { return error(err + 1); }
    s32 seq() const { return error(err + 2); }
    s32 busy() const { return error(err + 3); }
    s32 fatal() const { return error(err + 0x81); }
};

constexpr Core kAvc{"libavcdec", "_libvdec_avc", 0x80615200, 0xBE264DEC, 0x1380, 0x550, 46, 0x44, 36, 5, 20, 0x294300,
    0x295300, 0x298180, 0x299500, 0x2D9EA30D, "_pspocket_avcdec_thread", false};
constexpr Core kMvc{"libmvcdec", "_libvdec_mvc", 0x80615900, 0xBE264DDC, 0x1F80, 0x580, 82, 0x48, 36, 5, 36, 0x4F6400,
    0x4F7400, 0x4FA180, 0x4FC100, 0xC7AA1D3D, "_pspocket_mvcdec_thread", true};
constexpr u32 kMaxSideEntries = 82, kMaxFrameEntries = 36;

// The decoder's states (Sony's context +0x1a0).
enum State : u32 {
    ST_CLOSED = 0,
    ST_RESETTING = 1,
    ST_READY = 2,
    ST_DECODING = 3,
    ST_FLUSHING = 4,
    ST_FLUSHED = 5,
    ST_FAILED = 6,
};

// The message types the callback gets.
enum : u32 { MSG_AU_DONE = 0, MSG_FRAME = 1, MSG_FLUSH_DONE = 2, MSG_RESET_DONE = 3, MSG_SEVERE = 4 };

bool writable(PPUContext& ctx, u32 addr, u32 size) { return addr && guest_readable(thread_of(ctx), addr, size); }

// Another HLE library's function called as the core module calls its import (cellSpurs), on this thread: arguments past
// eight in the caller's parameter save area.
u64 call_import(PPUContext& ctx, std::string_view module, u32 nid, std::initializer_list<u64> args)
{
    const Function* f = registry().find(module, nid);
    if (!f) PS_FATAL(kChan, "{}'s function {:#010x} is not pspocket's own", module, nid);
    u64 saved[8];
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 0;
    for (const u64 a : args) {
        if (i < 8) ctx.gpr[3 + i] = a;
        else ps_write64(guest_base(ctx), static_cast<u32>(ctx.gpr[1]) + 0x70 + 8 * (i - 8), a);
        i++;
    }
    f->fn(ctx, guest_base(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

// cellSpurs's functions the core calls (their NIDs as pspocket's cellSpurs registers them).
enum : u32 {
    NID_cellSpursAttributeInitialize = 0x95180230, // _cellSpursAttributeInitialize
    NID_cellSpursAttributeSetNamePrefix = 0x07529113,
    NID_cellSpursInitializeWithAttribute = 0xAA6269A8,
    NID_cellSpursFinalize = 0xCA4C4600,
    NID_cellSpursTasksetAttributeInitialize = 0x16394A4E, // _cellSpursTasksetAttributeInitialize
    NID_cellSpursTasksetAttributeSetName = 0x652B70E2,
    NID_cellSpursTasksetAttributeSetTasksetSize = 0x8F122EF8,
    NID_cellSpursCreateTasksetWithAttribute = 0xC10931CB,
    NID_cellSpursShutdownTaskset = 0xA789E631,
    NID_cellSpursJoinTaskset = 0x9F72ADD3,
};

// The core's version as Sony's maps the game's SDK version (Open's seventh argument): 0, 0xC00 from SDK 1.5, 0x10600
// from 2.4, 0x10C00 from 3.3. From 0x10600 an access unit's pieces are limited to 0x1FFFFF80 bytes together; below
// 0x10C00 Open does not refuse parameters the memory query refuses.
u32 sdk_level(u32 sdk)
{
    u32 v = 0;
    for (const auto& [min, value] : {std::pair{0x150000u, 0xC00u}, {0x240000u, 0x10600u}, {0x330000u, 0x10C00u}})
        if (sdk >= min) v = value;
    return v;
}

// libavcdec's internal error codes as its PPU side gives them to the caller (the low six bits mapped, the top two
// kept; an unknown code is 0).
u8 external_error(u8 code)
{
    const u8 low = code & 0x3F;
    const bool known = low == 0 || (low >= 0x10 && low <= 0x18) || (low >= 0x20 && low <= 0x26) || low == 0x30;
    if (!known || low == 0) return 0;
    return static_cast<u8>(low | (code & 0xC0));
}

// =================================================================================================== the SPU side

// What Sony's SPU side reports of a picture it outputs (its frame output response), as the PPU side turns it into the
// information the callback gets.
struct FrameReport {
    u32 frame = 0; // the frame buffer
    u32 height = 0, width = 0;
    u32 crop_right = 0, crop_left = 0, crop_top = 0, crop_bottom = 0;
    u64 dts[2] = {0, 0}, pts[2] = {0, 0}; // each field's access unit's (as given; 0 for none)
    s32 poc[2] = {0, 0};
    u32 time_scale = 0, units_in_tick = 0;
    u16 sar_width = 0, sar_height = 0;
    u16 field_interval = 0;
    u8 skipped = 0, structure = 0, error = 0, frame_id = 0;
    u8 primaries = 0, aspect_ratio_idc = 0, transfer = 0, matrix = 0;
    u8 type[2] = {0, 0}; // each field's picture type: I 0, P 1, B 2
    u8 side[2] = {0, 0}; // each field's access unit's side data ID
    u8 decode_pair = 0, discard = 0;
    u8 cc_length[2] = {0, 0};
    u8 word5[4] = {0, 0, 0, 0};
    u8 coded_as = 0, video_format = 0, error_code = 0, gop_boundary = 0;
    u32 flags = 0;   // the stream's flags (sps, pps, aud present, VUI fields, ...)
    u32 packing = 0; // frame packing arrangement flags
    u8 colour_length[2] = {0, 0};
    u8 pan_scan_cancel[2] = {0, 0};
    std::array<u8, 0x20> packing_fields{}; // response +0x88 to +0xA7
    std::array<u8, 0x88> pan_scan{};       // response +0xA8 to +0x12F
    std::array<u8, 0x30> cc{};             // response +0x130 to +0x15F
    std::array<u8, 6> tail{};              // response +0x160 to +0x165
    // libmvcdec: the picture's view (0 the base view, 1 the second), whether it is its access unit's first picture,
    // and how many pictures its access unit gives
    u16 view = 0;
    u8 first_of_unit = 1, pictures = 1;
};

// Response flag bits (Sony's debug output names them).
enum : u32 {
    F_OTHER_SEI_ERROR = 1u << 2,
    F_FRAME_PACKING_SEI_ERROR = 1u << 3,
    F_PAN_SCAN_SEI_ERROR = 1u << 4,
    F_USER_DATA_SEI_ERROR = 1u << 5,
    F_BUFFERING_SEI_ERROR = 1u << 6,
    F_PIC_TIMING_SEI_ERROR = 1u << 7,
    F_IDR = 1u << 8,
    F_BUFFERING_SEI = 1u << 9,
    F_PIC_TIMING_SEI_1 = 1u << 10,
    F_PIC_TIMING_SEI_0 = 1u << 11,
    F_FILLER = 1u << 12,
    F_END_OF_STREAM = 1u << 13,
    F_END_OF_SEQ = 1u << 14,
    F_AUD = 1u << 15,
    F_PPS = 1u << 16,
    F_SPS = 1u << 17,
    F_FIXED_FRAME_RATE = 1u << 18,
    F_LOW_DELAY_HRD = 1u << 19,
    F_TIMING_INFO = 1u << 20,
    F_COLOUR_DESCRIPTION = 1u << 21,
    F_VIDEO_SIGNAL_TYPE = 1u << 22,
    F_FRAME_MBS_ONLY = 1u << 23,
    F_VUI = 1u << 24,
    F_CHROMA_FORMAT_SHIFT = 25,
    F_USER_DATA_SEI = 1u << 27,
    F_VCL_HRD = 1u << 28,
    F_NAL_HRD = 1u << 29,
    F_CABAC = 1u << 30,
    F_FULL_RANGE = 1u << 31,
};

// The information the callback gets of a picture (0x178 bytes): Sony's PPU side's arrangement of the SPU side's report
// (read in the module's frame output handler and checked byte for byte against it).
constexpr u32 kInfoBytes = 0x178;

void write_info(u8* base, u32 info, const FrameReport& r, u32 frame_id, u8 count, u32 side_ptr[2], u32 side_size[2])
{
    for (u32 i = 0; i < kInfoBytes; i++) ps_write8(base, info + i, 0);
    ps_write32(base, info + 0x00, frame_id);
    ps_write8(base, info + 0x04, r.skipped);
    ps_write8(base, info + 0x05, r.error);
    ps_write32(base, info + 0x08, external_error(r.error_code));
    ps_write8(base, info + 0x0C, r.gop_boundary);
    ps_write32(base, info + 0x10, r.height);
    ps_write32(base, info + 0x14, r.width);
    ps_write32(base, info + 0x18, r.crop_right);
    ps_write32(base, info + 0x1C, r.crop_left);
    ps_write32(base, info + 0x20, r.crop_top);
    ps_write32(base, info + 0x24, r.crop_bottom);
    ps_write8(base, info + 0x28, r.type[0]);
    ps_write8(base, info + 0x29, r.type[1]);
    ps_write8(base, info + 0x2A, r.aspect_ratio_idc);
    ps_write8(base, info + 0x2B, r.video_format);
    ps_write8(base, info + 0x2C, r.primaries);
    ps_write8(base, info + 0x2D, r.transfer);
    ps_write8(base, info + 0x2E, r.matrix);
    for (u32 i = 0; i < 0x18; i++) ps_write8(base, info + 0x2F + i, r.cc[i]);
    ps_write8(base, info + 0x47, r.cc_length[0]);
    ps_write8(base, info + 0x48, r.cc_length[1]);
    for (u32 i = 0; i < 0x18; i++) ps_write8(base, info + 0x49 + i, r.cc[0x18 + i]);
    ps_write8(base, info + 0x61, r.colour_length[0]);
    ps_write8(base, info + 0x62, r.colour_length[1]);
    ps_write8(base, info + 0x63, r.structure);
    ps_write8(base, info + 0x64, r.coded_as);
    for (u32 i = 0; i < 6; i++) ps_write8(base, info + 0x65 + i, r.tail[i]);
    ps_write8(base, info + 0x6B, count);
    ps_write8(base, info + 0x6C, r.word5[3]);
    ps_write64(base, info + 0x70, r.dts[0]);
    ps_write64(base, info + 0x78, r.dts[1]);
    ps_write64(base, info + 0x80, r.pts[0]);
    ps_write64(base, info + 0x88, r.pts[1]);
    ps_write16(base, info + 0x90, r.field_interval);
    ps_write32(base, info + 0x94, side_ptr[0]);
    ps_write32(base, info + 0x98, side_ptr[1]);
    ps_write32(base, info + 0x9C, side_size[0]);
    ps_write32(base, info + 0xA0, side_size[1]);
    ps_write32(base, info + 0xA4, r.frame);
    ps_write32(base, info + 0xA8, static_cast<u32>(r.poc[0]));
    ps_write32(base, info + 0xAC, static_cast<u32>(r.poc[1]));
    ps_write32(base, info + 0xB0, r.time_scale);
    ps_write32(base, info + 0xB4, r.units_in_tick);
    for (u32 i = 0; i < 0x10; i++) ps_write8(base, info + 0xB8 + i, r.pan_scan[i]);
    ps_write8(base, info + 0xC8, r.pan_scan_cancel[0]);
    ps_write8(base, info + 0xC9, r.pan_scan_cancel[1]);
    for (u32 i = 0; i < 0x78; i++) ps_write8(base, info + 0xCC + i, r.pan_scan[0x10 + i]);
    // The frame packing arrangement's fields (response +0x88 to +0xA7).
    const auto pf = [&](u32 off) { return r.packing_fields[off - 0x88]; };
    for (u32 i = 0; i < 8; i++) ps_write8(base, info + 0x144 + i, pf(0xA0 + i));
    for (u32 i = 0; i < 4; i++) ps_write8(base, info + 0x14C + i, pf(0x98 + i));
    for (u32 k = 0; k < 12; k++) ps_write8(base, info + 0x150 + k, static_cast<u8>((r.packing >> (11 - k)) & 1));
    ps_write8(base, info + 0x15C, pf(0x9C));
    ps_write8(base, info + 0x15D, pf(0x9D));
    for (u32 i = 0; i < 8; i++) ps_write8(base, info + 0x15E + i, pf(0x90 + i));
    ps_write8(base, info + 0x166, static_cast<u8>((r.packing >> 15) & 1));
    ps_write8(base, info + 0x167, static_cast<u8>((r.packing >> 14) & 1));
    for (u32 i = 0; i < 4; i++) ps_write8(base, info + 0x168 + i, pf(0x88 + i));
    ps_write8(base, info + 0x16C, pf(0x8E));
    ps_write8(base, info + 0x16D, pf(0x8F));
    ps_write8(base, info + 0x16E, pf(0x8C));
    ps_write8(base, info + 0x16F, pf(0x8D));
    ps_write16(base, info + 0x170, r.sar_width);
    ps_write16(base, info + 0x172, r.sar_height);
    // The flags, rearranged.
    const u32 f = r.flags;
    const auto bit = [&](u32 word, u32 n) { return (word >> n) & 1; };
    u8 b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    for (u32 n = 0; n < 8; n++) b0 |= static_cast<u8>(bit(f, 10 + n) << n);
    b1 = static_cast<u8>(bit(f, 6) | bit(f, 7) << 1 | bit(r.packing, 12) << 2 | bit(r.packing, 13) << 3 | bit(f, 0) << 4 |
                         bit(f, 1) << 5 | bit(f, 27) << 6 | bit(f, 9) << 7);
    b2 = static_cast<u8>(bit(f, 21) | bit(f, 22) << 1 | bit(f, 23) << 2 | bit(f, 24) << 3 | bit(f, 2) << 4 | bit(f, 3) << 5 |
                         bit(f, 4) << 6 | bit(f, 5) << 7);
    b3 = static_cast<u8>(bit(f, 28) | bit(f, 29) << 1 | bit(f, 30) << 2 | bit(f, 31) << 3 | bit(f, 8) << 4 | bit(f, 18) << 5 |
                         bit(f, 19) << 6 | bit(f, 20) << 7);
    ps_write8(base, info + 0x174, b0);
    ps_write8(base, info + 0x175, b1);
    ps_write8(base, info + 0x176, b2);
    ps_write8(base, info + 0x177, b3);
}

// libmvcdec's information of a picture (0x158 bytes after its message's 0x44): libavcdec's fields with the view's
// (its index, whether it is the first of its access unit's, how many pictures the access unit has) where libavcdec has
// its GOP flag (which follows them), the fields after it moved by 4 and those from the DTS on by 8, no frame packing,
// and the flags as one doubleword: the SPU side's flags (libavcdec's one bit down) spread over its bits 33 to 63 (the
// low bits as the helper thread's stack had them: not written).
constexpr u32 kMvcInfoBytes = 0x158;

void write_mvc_info(u8* base, u32 info, const FrameReport& r, u32 frame_id, u8 count, u32 side_ptr[2], u32 side_size[2],
    u16 view, u8 first_of_unit, u8 pictures)
{
    for (u32 i = 0; i < kMvcInfoBytes; i++) ps_write8(base, info + i, 0);
    ps_write32(base, info + 0x00, frame_id);
    ps_write8(base, info + 0x04, r.skipped);
    ps_write8(base, info + 0x05, r.error);
    ps_write32(base, info + 0x08, external_error(r.error_code));
    ps_write16(base, info + 0x0C, view);
    ps_write8(base, info + 0x0E, first_of_unit);
    ps_write8(base, info + 0x0F, pictures);
    ps_write8(base, info + 0x10, r.gop_boundary);
    ps_write32(base, info + 0x14, r.height);
    ps_write32(base, info + 0x18, r.width);
    ps_write32(base, info + 0x1C, r.crop_right);
    ps_write32(base, info + 0x20, r.crop_left);
    ps_write32(base, info + 0x24, r.crop_top);
    ps_write32(base, info + 0x28, r.crop_bottom);
    ps_write8(base, info + 0x2C, r.type[0]);
    ps_write8(base, info + 0x2D, r.type[1]);
    ps_write8(base, info + 0x2E, r.aspect_ratio_idc);
    ps_write8(base, info + 0x2F, r.video_format);
    ps_write8(base, info + 0x30, r.primaries);
    ps_write8(base, info + 0x31, r.transfer);
    ps_write8(base, info + 0x32, r.matrix);
    for (u32 i = 0; i < 0x18; i++) ps_write8(base, info + 0x33 + i, r.cc[i]);
    ps_write8(base, info + 0x4B, r.cc_length[0]);
    ps_write8(base, info + 0x4C, r.cc_length[1]);
    for (u32 i = 0; i < 0x18; i++) ps_write8(base, info + 0x4D + i, r.cc[0x18 + i]);
    ps_write8(base, info + 0x65, r.colour_length[0]);
    ps_write8(base, info + 0x66, r.colour_length[1]);
    ps_write8(base, info + 0x67, r.structure);
    ps_write8(base, info + 0x68, r.coded_as);
    for (u32 i = 0; i < 6; i++) ps_write8(base, info + 0x69 + i, r.tail[i]);
    ps_write8(base, info + 0x6F, count);
    ps_write8(base, info + 0x70, r.word5[3]);
    ps_write64(base, info + 0x78, r.dts[0]);
    ps_write64(base, info + 0x80, r.dts[1]);
    ps_write64(base, info + 0x88, r.pts[0]);
    ps_write64(base, info + 0x90, r.pts[1]);
    ps_write16(base, info + 0x98, r.field_interval);
    ps_write32(base, info + 0x9C, side_ptr[0]);
    ps_write32(base, info + 0xA0, side_ptr[1]);
    ps_write32(base, info + 0xA4, side_size[0]);
    ps_write32(base, info + 0xA8, side_size[1]);
    ps_write32(base, info + 0xAC, r.frame);
    ps_write32(base, info + 0xB0, static_cast<u32>(r.poc[0]));
    ps_write32(base, info + 0xB4, static_cast<u32>(r.poc[1]));
    ps_write32(base, info + 0xB8, r.time_scale);
    ps_write32(base, info + 0xBC, r.units_in_tick);
    for (u32 i = 0; i < 0x10; i++) ps_write8(base, info + 0xC0 + i, r.pan_scan[i]);
    ps_write8(base, info + 0xD0, r.pan_scan_cancel[0]);
    ps_write8(base, info + 0xD1, r.pan_scan_cancel[1]);
    for (u32 i = 0; i < 0x78; i++) ps_write8(base, info + 0xD4 + i, r.pan_scan[0x10 + i]);
    ps_write16(base, info + 0x14C, r.sar_width);
    ps_write16(base, info + 0x14E, r.sar_height);
    // the flags: response bit k (libavcdec's bit k + 1) at these bits of the doubleword
    static constexpr std::array<u8, 31> kAt{48, 49, 47, 50, 51, 52, 53, 39, 55, 56, 57, 58, 59, 60, 61, 62, 63, 40, 41, 42,
        43, 44, 45, 46, 33, 34, 54, 35, 36, 37, 38};
    u64 flags = 0;
    for (u32 k = 0; k < kAt.size(); k++)
        if ((r.flags >> (k + 1)) & 1) flags |= u64{1} << kAt[k];
    ps_write32(base, info + 0x150, static_cast<u32>(flags >> 32));
}

// What the SPU side says of an access unit when it is done with it (its AU complete response).
struct AuReport {
    u32 addr = 0, cont = 0;
    // libmvcdec's message has every piece and its size, and how many pictures the access unit gives
    u32 size = 0, cont_size = 0, dep = 0, dep_size = 0, dep_cont = 0, dep_cont_size = 0;
    u8 pictures = 1;
    u8 discarded = 0, skipped = 0, error = 0, side_id = 0;
    u8 error_code = 0, bd_cv = 0, bd_decodable = 1;
};

// A picture in the decoder: a frame, or a field pair (the second field's access unit joins the first's picture).
struct Picture {
    u32 slot = 0, frame = 0;
    u32 width = 0, height = 0;
    bool idr = false, reference = false, long_term = false, non_existing = false;
    bool output_done = false, in_dpb = false;
    bool mmco5 = false, no_output_of_prior_pics = false;
    u32 frame_num = 0;
    s32 frame_num_wrap = 0;
    u32 long_term_frame_idx = 0;
    s32 poc = 0;
    u32 fields = 3; // 1 top, 2 bottom, 3 both (a frame)
    // When the picture may be output by the clock: its PTS, when its access unit's DTS and PTS are both given and the
    // PTS is not before the DTS; else none (it leaves the buffer when the buffer outputs it).
    std::optional<u64> out_time;
    bool adaptive = false;
    std::vector<Mmco> mmco;
    FrameReport report;
    // libmvcdec: a base view picture's second view picture (in a frame buffer of its own, output right after it), and
    // a second view picture's base view picture (it is in the buffer while that is)
    std::shared_ptr<Picture> second;
    std::weak_ptr<Picture> first;
};
using PicturePtr = std::shared_ptr<Picture>;

// Whether a picture is in the decoded picture buffer (a second view picture: its base view picture).
bool in_buffer(const Picture& p)
{
    if (p.in_dpb) return true;
    if (const PicturePtr f = p.first.lock()) return f->in_dpb;
    return false;
}

constexpr u32 kNoSlot = ~0u; // a picture with no frame buffer (libmvcdec's skipped pictures)

// Whether `slot` is a picture's (or its second view picture's) frame buffer.
bool uses_slot(const Picture& p, u32 slot) { return p.slot == slot || (p.second && p.second->slot == slot); }

// A command for the SPU side, as libavcdec's PPU side queues them.
struct Command {
    enum class Kind { Au, Flush, Release, Reset, Close } kind = Kind::Au;
    // an access unit (libmvcdec's with its second view's pieces)
    u32 addr = 0, size = 0, cont = 0, cont_size = 0;
    u32 dep = 0, dep_size = 0, dep_cont = 0, dep_cont_size = 0;
    u64 dts = 0, pts = 0;
    u32 mode = 0;
    u8 side_id = 0;
    // a release
    u8 count = 0, side[2] = {0, 0}, frame_id = 0, skipped = 0;
    // a reset
    u32 keep_sps = 0, reset_flags = 0;
};

// One message for the callback, built when the SPU side answers (the callback is called with no lock held).
struct Message {
    u32 type = 0;
    AuReport au;
    FrameReport frame;
};

#if PSPOCKET_HAVE_FFMPEG
// FFmpeg's H.264 decoder as the source of the samples: every picture's buffer is captured when FFmpeg takes it, so its
// samples are read as soon as its access unit is decoded, whatever FFmpeg's own output order.
class Samples {
public:
    ~Samples() { close(); }
    bool open()
    {
        close();
        const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!dec) return false;
        av_ = avcodec_alloc_context3(dec);
        av_->thread_count = 1;
        av_->apply_cropping = 0;
        av_->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
        av_->opaque = this;
        av_->get_buffer2 = get_buffer;
        if (avcodec_open2(av_, dec, nullptr) < 0) {
            avcodec_free_context(&av_);
            return false;
        }
        frame_ = av_frame_alloc();
        out_ = av_frame_alloc();
        return true;
    }
    void close()
    {
        if (frame_) av_frame_free(&frame_);
        if (out_) av_frame_free(&out_);
        avcodec_free_context(&av_);
    }
    bool is_open() const { return av_ != nullptr; }
    void reset()
    {
        if (av_) avcodec_flush_buffers(av_);
        if (frame_) av_frame_unref(frame_);
        captured_ = false;
    }
    // Decodes one access unit; `skip_nonref` leaves its picture undecoded when it is not a reference. Whether a new
    // picture's buffer was taken (its samples are then in last()).
    bool decode(const u8* data, u32 size, bool skip_nonref, bool& damaged)
    {
        damaged = false;
        if (!av_) return false;
        captured_ = false;
        std::vector<u8> copy(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        std::memcpy(copy.data(), data, size);
        AVPacket* pkt = av_packet_alloc();
        pkt->data = copy.data();
        pkt->size = static_cast<int>(size);
        av_->skip_frame = skip_nonref ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
        const int r = avcodec_send_packet(av_, pkt);
        av_packet_free(&pkt);
        while (avcodec_receive_frame(av_, out_) == 0) {
            if (out_->decode_error_flags || (out_->flags & AV_FRAME_FLAG_CORRUPT)) damaged_any_ = true;
            av_frame_unref(out_);
        }
        damaged = r < 0;
        return captured_;
    }
    // The last picture FFmpeg decoded into: its planes at its coded size (planar 4:2:0, no padding).
    bool planes(u32 width, u32 height, std::vector<u8>& y, std::vector<u8>& u, std::vector<u8>& v) const
    {
        if (!frame_ || !frame_->data[0]) return false;
        const auto fmt = static_cast<AVPixelFormat>(frame_->format);
        if (fmt != AV_PIX_FMT_YUV420P && fmt != AV_PIX_FMT_YUVJ420P) return false;
        if (frame_->width < static_cast<int>(width) || frame_->height < static_cast<int>(height)) return false;
        y.resize(width * height);
        u.resize(width * height / 4);
        v.resize(width * height / 4);
        for (u32 r = 0; r < height; r++)
            std::memcpy(&y[r * width], frame_->data[0] + r * static_cast<u32>(frame_->linesize[0]), width);
        for (u32 r = 0; r < height / 2; r++) {
            std::memcpy(&u[r * width / 2], frame_->data[1] + r * static_cast<u32>(frame_->linesize[1]), width / 2);
            std::memcpy(&v[r * width / 2], frame_->data[2] + r * static_cast<u32>(frame_->linesize[2]), width / 2);
        }
        return true;
    }

private:
    static int get_buffer(AVCodecContext* av, AVFrame* f, int flags)
    {
        const int r = avcodec_default_get_buffer2(av, f, flags);
        auto* self = static_cast<Samples*>(av->opaque);
        if (r >= 0 && self && self->frame_) {
            av_frame_unref(self->frame_);
            if (av_frame_ref(self->frame_, f) >= 0) self->captured_ = true;
        }
        return r;
    }
    AVCodecContext* av_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* out_ = nullptr;
    bool captured_ = false;
    bool damaged_any_ = false;
};
#endif

// The decoder: its PPU side's state (what the caller's calls and the helper thread share, under m) and its SPU side's
// (the helper thread's alone).
struct Decoder {
    const Core* core = &kAvc;
    std::mutex m;
    u32 handle = 0, mem = 0, mem_size = 0;
    Layout lay;
    u32 level = 0, deblock = 0, buffers = 0, width_mbs = 0, height_mbs = 0;
    u32 sdk = 0;
    u32 callback = 0, cb_arg = 0;
    bool own_spurs = false;
    u32 spurs = 0, taskset = 0;
    u64 thread_id = 0;
    u32 tls = 0;
    // PPU side (under m)
    State state = ST_CLOSED;
    u16 seq = 0;
    u8 side_counter = 0, skip_counter = 0;
    u32 in_flight = 0;
    bool closing = false, shutting_down = false;
    bool destroyed = false; // closed: its critical section is gone, so the calls that lock it get lv2's ESRCH
    struct SideEntry {
        bool used = false;
        u32 key = 0, size = 0;
        // libmvcdec's: the releases its pictures need (0xFF: none frees it), those taken, and whether its access unit
        // is done (the entry is freed when both are so)
        u8 expected = 0xFF, released = 0;
        bool done = false;
    };
    std::array<SideEntry, kMaxSideEntries> side{};
    struct FrameEntry {
        bool used = false;
        u32 key = 0;
        u8 count = 0, skipped = 0;
        u32 side[2] = {0, 0};
    };
    std::array<FrameEntry, kMaxFrameEntries> frames{};
    std::deque<Command> au_queue, other_queue;
    lv2::WaitQueue waiters; // the helper thread, waiting for a command
    bool thread_started = false;

    u8 views = 0; // libmvcdec: the first access unit's views (1, or 2 with the second view's piece); 0 before it
    // libmvcdec: a picture output at once (stored never), whose message comes with the next access unit's: one view's
    // (after its AU done), or with two views a second view picture (before it; its base view picture's came at once)
    PicturePtr pending_second;

    // SPU side (the helper thread)
    u32 reset_flags = 3;         // Reset's flags: 0 refuses every sequence
    bool seq_valid = false;      // a sequence the core decodes is active
    bool error_reported = false; // the missing or refused sequence's error code was given: later access units give 0
    bool sps_pending = false;    // an SPS came since the last I picture (the next one starts a GOP)
    // The last decoded picture's type and order count: an error picture within a sequence reports them.
    u8 last_type = 0;
    s32 last_poc = 0;
    bool last_failed = false; // libmvcdec: the access unit before was one in error
    std::array<std::optional<Sps>, 32> sps{};
    std::array<std::optional<Pps>, 256> pps{};
    std::optional<Sps> active;
    PocState poc_state;
    u32 prev_ref_frame_num = 0;
    std::vector<PicturePtr> dpb;
    u32 dpb_size = 0;
    PicturePtr current; // decoded, put in the buffer when the next picture's access unit is decoded
    std::optional<Command> stalled; // an access unit waiting for a free frame buffer
    // The access unit under way: the previous picture stored already (its outputs queued until the access unit is
    // done), so an access unit that waits for a frame buffer does not store it again.
    bool previous_stored = false;
    std::vector<PicturePtr> queued;
    u32 slots = 0, slot_bytes = 0, last_slot = 0;
    std::vector<bool> slot_used; // a decoded picture was in it since the layout
    u32 frame_width = 0, frame_height = 0;
    // The pictures given to the caller whose frame ID is not free yet: an ID is free again once the caller released it
    // (and the release was taken) and the picture has left the buffer (it is no reference any more).
    struct Output {
        u32 id = 0;
        PicturePtr pic;
        bool released = false;
    };
    std::vector<Output> outputs;
#if PSPOCKET_HAVE_FFMPEG
    Samples samples;
#endif
    // libmvcdec's second view: its decoder, and its subset SPS (its pictures' sequence fields)
    SecondView second_view;
    std::array<std::optional<Sps>, 32> subset{};
};

struct Decoders {
    std::mutex m;
    std::map<u32, std::shared_ptr<Decoder>> by_handle;
};

Decoders& decoders()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "libavcdec called without an HLE process");
    return p->state<Decoders>();
}

std::shared_ptr<Decoder> decoder_at(u32 handle)
{
    Decoders& all = decoders();
    std::lock_guard lk(all.m);
    const auto it = all.by_handle.find(handle);
    return it == all.by_handle.end() ? nullptr : it->second;
}

// Wakes the helper thread. Under d.m.
void wake(Decoder& d)
{
    for (lv2::Waiter* w : d.waiters.take_all(lv2::SYS_SYNC_FIFO)) lv2::wake(*w, lv2::CELL_OK);
}

template <class Ready>
void park_until(lv2::PpuThread& t, Decoder& d, std::unique_lock<std::mutex>& lk, Ready ready)
{
    while (!ready()) {
        lv2::prepare_wait(t.waiter);
        d.waiters.push(&t.waiter);
        const lv2::WaitStatus status = lv2::park(t.waiter, lk, 0, {lv2::WaitKind::Sleep, 0}, [&](bool) {
            return d.waiters.remove(&t.waiter) ? lv2::CancelResult::Removed : lv2::CancelResult::Woken;
        });
        if (status == lv2::WaitStatus::Aborted) throw lv2::ThreadExit{};
        lk.lock();
    }
}

// ---------------------------------------------------------------------------------------------------- frame buffers

// An error picture's frame buffer before any are laid out, as Sony's core leaves it: the luma and the gap after it 0,
// the chroma and its gap 0x80.
void fill_error_frame(u8* base, u32 at, u32 width, u32 height)
{
    const u32 luma = ((width + 15) / 16 + 4) * 16 * height + 0x400;
    const u32 bytes = frame_slot_bytes(width, height);
    std::memset(base + at, 0, luma);
    std::memset(base + at + luma, 0x80, bytes - luma);
}

u32 slot_addr(const Decoder& d, u32 slot) { return d.mem + d.core->dynamic_at + d.lay.frames + slot * d.slot_bytes; }

// The frame buffers for pictures of this size: the slots past the SPU side's first four blocks, as many as the decoded
// picture buffer and two more, each the picture's size (Sony's decoder lays them out again when the size changes).
void lay_out_frames(Decoder& d, u32 width, u32 height, u32 dpb)
{
    d.frame_width = width;
    d.frame_height = height;
    d.slot_bytes = frame_slot_bytes(width, height);
    d.slots = (dpb + 2) * (d.views == 2 ? 2 : 1); // libmvcdec's two views: each picture's two frame buffers
    d.last_slot = 0;
    d.slot_used.assign(d.slots, false);
}

// Whether the frame buffer `slot` holds a picture still needed: in the decoded picture buffer, the picture decoded
// last, one output whose message waits, or one whose frame ID is not free yet.
bool slot_busy(const Decoder& d, u32 slot)
{
    for (const PicturePtr& p : d.dpb)
        if (uses_slot(*p, slot)) return true;
    if (d.current && uses_slot(*d.current, slot)) return true;
    for (const PicturePtr& p : d.queued)
        if (uses_slot(*p, slot)) return true;
    for (const Decoder::Output& o : d.outputs)
        if (o.pic->slot == slot) return true;
    return false;
}

// The next free frame buffer after the last one taken, in turn; none when every one is busy. `was_free`: the buffers
// free before the access unit's buffer step (one a picture leaving the buffer in that step frees is not taken yet).
std::optional<u32> take_slot(Decoder& d, const std::vector<bool>& was_free = {})
{
    for (u32 k = 1; k <= d.slots; k++) {
        const u32 s = (d.last_slot + k) % d.slots;
        if (!slot_busy(d, s) && (was_free.empty() || (s < was_free.size() && was_free[s]))) {
            d.last_slot = s;
            return s;
        }
    }
    return std::nullopt;
}

// The frame IDs free again: released, and their pictures out of the buffer.
void free_ids(Decoder& d)
{
    std::erase_if(d.outputs, [](const Decoder::Output& o) { return o.released && !in_buffer(*o.pic); });
}

// The lowest frame ID not in use.
u32 take_frame_id(Decoder& d, const PicturePtr& p)
{
    u32 id = 0;
    for (bool taken = true; taken; id += taken ? 1 : 0) {
        taken = false;
        for (const Decoder::Output& o : d.outputs)
            if (o.id == id) taken = true;
    }
    d.outputs.push_back({id, p, false});
    return id;
}

// ---------------------------------------------------------------------------------------------------- the DPB

// The decoded picture buffer's size for a sequence (A.3.1): the VUI's max_dec_frame_buffering when the stream gives it
// (at least 1), else the level's MaxDpbMbs over the frame's macroblocks, at most 16.
u32 dpb_size_of(const Sps& s)
{
    if (s.vui_present && s.vui.bitstream_restriction) return std::max<u32>(1, s.vui.max_dec_frame_buffering);
    u32 max_dpb_mbs = 0;
    switch (s.level_idc) {
    case 9: max_dpb_mbs = 396; break;
    case 10: max_dpb_mbs = 396; break;
    case 11: max_dpb_mbs = (s.constraints & 0x10) && s.profile_idc != 100 ? 396 : 900; break;
    case 12:
    case 13:
    case 20: max_dpb_mbs = 2376; break;
    case 21: max_dpb_mbs = 4752; break;
    case 22:
    case 30: max_dpb_mbs = 8100; break;
    case 31: max_dpb_mbs = 18000; break;
    case 32: max_dpb_mbs = 20480; break;
    case 40:
    case 41: max_dpb_mbs = 32768; break;
    case 42: max_dpb_mbs = 34816; break;
    case 50: max_dpb_mbs = 110400; break;
    case 51:
    case 52: max_dpb_mbs = 184320; break;
    default: max_dpb_mbs = 32768; break;
    }
    const u32 frame_mbs = s.width_mbs * s.frame_height_mbs();
    return std::clamp<u32>(frame_mbs ? max_dpb_mbs / frame_mbs : 16, 1, 16);
}

void remove_from_dpb(Decoder& d, const PicturePtr& p)
{
    std::erase(d.dpb, p);
    p->in_dpb = false;
}

// A picture output; one that is no reference leaves the buffer.
void output(Decoder& d, const PicturePtr& p, std::vector<PicturePtr>& out)
{
    p->output_done = true;
    out.push_back(p);
    if (!p->reference) remove_from_dpb(d, p);
}

// The picture not yet output with the smallest order count; none when every one was output.
PicturePtr smallest_poc(const Decoder& d)
{
    PicturePtr best;
    for (const PicturePtr& p : d.dpb)
        if (!p->output_done && (!best || p->poc < best->poc)) best = p;
    return best;
}

// Pictures output (smallest count first) until the buffer holds no more than its size; output pictures that are no
// reference left it already.
void bump(Decoder& d, std::vector<PicturePtr>& out)
{
    while (d.dpb.size() > d.dpb_size) {
        const PicturePtr p = smallest_poc(d);
        if (!p) break;
        output(d, p, out);
    }
}

constexpr u64 kNoTime = ~0ull; // a DTS or PTS not given

// The pictures whose output time `now` has reached leave by the clock, smallest count first.
void output_by_time(Decoder& d, u64 now, std::vector<PicturePtr>& out)
{
    for (;;) {
        PicturePtr best;
        for (const PicturePtr& p : d.dpb)
            if (!p->output_done && p->out_time && *p->out_time <= now && (!best || p->poc < best->poc)) best = p;
        if (!best) return;
        output(d, best, out);
    }
}

// Every picture output in order of their counts, the buffer emptied (an IDR picture, memory management operation 5,
// the flush).
void flush_dpb(Decoder& d, std::vector<PicturePtr>& out)
{
    for (const PicturePtr& p : d.dpb) p->reference = p->long_term = false;
    while (const PicturePtr p = smallest_poc(d)) output(d, p, out);
    for (const PicturePtr& p : d.dpb) p->in_dpb = false;
    d.dpb.clear();
}

// A reference picture no more: out of the buffer when it was output.
void unmark(Decoder& d, const PicturePtr& p)
{
    p->reference = p->long_term = false;
    if (p->output_done) remove_from_dpb(d, p);
}

// The frame_num wrap of the short-term references against the current picture's frame_num (8.2.4.1).
void update_wraps(Decoder& d, const Sps& s, u32 frame_num)
{
    for (const PicturePtr& p : d.dpb) {
        if (!p->reference || p->long_term) continue;
        p->frame_num_wrap = p->frame_num > frame_num ? static_cast<s32>(p->frame_num) - static_cast<s32>(s.max_frame_num())
                                                     : static_cast<s32>(p->frame_num);
    }
}

// The sliding window (8.2.5.3) before a reference picture: the oldest short-term reference unmarked while the
// references fill max_num_ref_frames.
void sliding_window(Decoder& d, const Sps& s)
{
    const u32 max_refs = std::max<u32>(1, s.max_num_ref_frames);
    for (;;) {
        u32 refs = 0;
        PicturePtr oldest;
        for (const PicturePtr& p : d.dpb) {
            if (!p->reference) continue;
            refs++;
            if (!p->long_term && (!oldest || p->frame_num_wrap < oldest->frame_num_wrap)) oldest = p;
        }
        if (refs < max_refs || !oldest) return;
        unmark(d, oldest);
    }
}

// The memory management operations of picture `p` (8.2.5.4).
void adaptive_marking(Decoder& d, const PicturePtr& p)
{
    const s32 cur_pic_num = static_cast<s32>(p->frame_num);
    for (const Mmco& m : p->mmco) {
        const std::vector<PicturePtr> refs = d.dpb;
        switch (m.op) {
        case 1: {
            const s32 pic_num = cur_pic_num - static_cast<s32>(m.difference_of_pic_nums_minus1 + 1);
            for (const PicturePtr& q : refs)
                if (q->reference && !q->long_term && q->frame_num_wrap == pic_num) unmark(d, q);
            break;
        }
        case 2:
            for (const PicturePtr& q : refs)
                if (q->reference && q->long_term && q->long_term_frame_idx == m.long_term_pic_num) unmark(d, q);
            break;
        case 3: {
            const s32 pic_num = cur_pic_num - static_cast<s32>(m.difference_of_pic_nums_minus1 + 1);
            for (const PicturePtr& q : refs)
                if (q->reference && q->long_term && q->long_term_frame_idx == m.long_term_frame_idx) unmark(d, q);
            for (const PicturePtr& q : d.dpb)
                if (q->reference && !q->long_term && q->frame_num_wrap == pic_num) {
                    q->long_term = true;
                    q->long_term_frame_idx = m.long_term_frame_idx;
                }
            break;
        }
        case 4:
            for (const PicturePtr& q : refs)
                if (q->reference && q->long_term && q->long_term_frame_idx + 1 > m.max_long_term_frame_idx_plus1) unmark(d, q);
            break;
        case 5:
            for (const PicturePtr& q : refs) unmark(d, q);
            p->mmco5 = true;
            break;
        case 6:
            for (const PicturePtr& q : refs)
                if (q->reference && q->long_term && q->long_term_frame_idx == m.long_term_frame_idx) unmark(d, q);
            p->long_term = true;
            p->long_term_frame_idx = m.long_term_frame_idx;
            break;
        default: break;
        }
    }
}

// =================================================================================================== the helper thread


// The access unit's bytes: its piece and its continuation.
std::vector<u8> au_bytes(u8* base, const Command& c)
{
    std::vector<u8> out(base + c.addr, base + c.addr + c.size);
    if (c.cont && c.cont_size) out.insert(out.end(), base + c.cont, base + c.cont + c.cont_size);
    return out;
}

// What the stream says of the sequence, for a picture's report (the flags and the VUI's fields).
void sequence_fields(const Sps& s, FrameReport& r, bool mvc = false)
{
    r.width = s.width();
    r.height = s.height();
    if (s.cropping) {
        r.crop_left = s.crop_left * s.crop_unit_x();
        r.crop_right = s.crop_right * s.crop_unit_x();
        r.crop_top = s.crop_top * s.crop_unit_y();
        r.crop_bottom = s.crop_bottom * s.crop_unit_y();
    }
    if (s.frame_mbs_only) r.flags |= F_FRAME_MBS_ONLY;
    r.flags |= (s.chroma_format_idc & 3) << F_CHROMA_FORMAT_SHIFT;
    if (!s.vui_present) {
        // No VUI: the values the standard infers (unspecified video format, colour primaries, transfer and matrix);
        // a VUI that lacks the video signal type leaves them 0.
        r.video_format = 5;
        r.primaries = r.transfer = r.matrix = 2;
    } else {
        const Vui& v = s.vui;
        r.flags |= F_VUI;
        if (v.aspect_ratio_info) {
            r.aspect_ratio_idc = v.aspect_ratio_idc;
            if (v.aspect_ratio_idc == 255) {
                r.sar_width = v.sar_width;
                r.sar_height = v.sar_height;
            }
        }
        if (mvc) {
            // libmvcdec's SPU side infers what the VUI leaves out (libavcdec's leaves it 0)
            r.video_format = 5;
            r.primaries = r.transfer = r.matrix = 2;
        }
        if (v.video_signal_type) {
            r.flags |= F_VIDEO_SIGNAL_TYPE;
            r.video_format = v.video_format;
            if (v.full_range) r.flags |= F_FULL_RANGE;
            if (v.colour_description) {
                r.flags |= F_COLOUR_DESCRIPTION;
                r.primaries = v.primaries;
                r.transfer = v.transfer;
                r.matrix = v.matrix;
            }
        }
        if (v.timing_info) {
            r.flags |= F_TIMING_INFO;
            r.time_scale = v.time_scale;
            r.units_in_tick = v.units_in_tick;
            if (v.fixed_frame_rate) r.flags |= F_FIXED_FRAME_RATE;
        }
        if (v.nal_hrd) r.flags |= F_NAL_HRD;
        if (v.vcl_hrd) r.flags |= F_VCL_HRD;
        if (v.low_delay_hrd) r.flags |= F_LOW_DELAY_HRD;
    }
}

// The messages a picture leaving the decoder makes: its frame ID taken.
void emit_output(Decoder& d, const PicturePtr& p, std::vector<Message>& msgs, bool with_second = true)
{
    Message m;
    m.type = MSG_FRAME;
    m.frame = p->report;
    m.frame.frame = p->frame;
    m.frame.frame_id = static_cast<u8>(take_frame_id(d, p));
    msgs.push_back(m);
    if (p->second && with_second) emit_output(d, p->second, msgs);
}

// The caller's releases taken (they free frame IDs and frame buffers): the SPU side takes them when it starts an access
// unit, or while one waits for a frame buffer.
void take_releases(Decoder& d)
{
    std::lock_guard lk(d.m);
    // A skipped picture's ID (the PPU side gives the caller 256 and on instead) is released by itself when the next
    // access unit starts: the caller's release of it carries no ID the SPU side knows.
    for (Decoder::Output& o : d.outputs)
        if (o.pic->report.skipped && !o.pic->report.error) o.released = true;
    for (auto it = d.other_queue.begin(); it != d.other_queue.end();) {
        if (it->kind != Command::Kind::Release) {
            ++it;
            continue;
        }
        if (!it->skipped)
            for (Decoder::Output& o : d.outputs)
                if (o.id == it->frame_id) o.released = true;
        it = d.other_queue.erase(it);
    }
    free_ids(d);
}

// What an access unit holds, read before anything of the decoder changes (an access unit that waits for a frame
// buffer is read again when it goes on).
struct AuContent {
    std::vector<u8> data;
    std::vector<Nal> nals;
    std::vector<Sps> sps;
    std::vector<Pps> pps;
    std::vector<SeiMessage> sei;
    u32 flags = 0;
    std::optional<SliceHeader> first;
    const Nal* first_nal = nullptr;
    const Sps* first_sps = nullptr;
    const Pps* first_pps = nullptr;
};

void read_au(u8* base, const Decoder& d, const Command& c, AuContent& a)
{
    a.data = au_bytes(base, c);
    a.nals = split_nals(a.data);
    // The parameter sets as they stand at each slice: the decoder's, then this access unit's in order.
    std::array<const Sps*, 32> spss{};
    std::array<const Pps*, 256> ppss{};
    for (usize i = 0; i < 32; i++) spss[i] = d.sps[i] ? &*d.sps[i] : nullptr;
    for (usize i = 0; i < 256; i++) ppss[i] = d.pps[i] ? &*d.pps[i] : nullptr;
    a.sps.reserve(a.nals.size());
    a.pps.reserve(a.nals.size());
    for (const Nal& n : a.nals) {
        switch (n.type) {
        case NAL_SPS: {
            Rbsp b = payload_of(n);
            a.sps.push_back(parse_sps(b));
            const Sps& s = a.sps.back();
            if (s.valid && s.id < 32) spss[s.id] = &s;
            a.flags |= F_SPS;
            break;
        }
        case NAL_PPS: {
            Rbsp b = payload_of(n);
            a.pps.push_back(parse_pps(b));
            const Pps& p = a.pps.back();
            if (p.valid && p.id < 256) ppss[p.id] = &p;
            a.flags |= F_PPS;
            break;
        }
        case NAL_AUD: a.flags |= F_AUD; break;
        case NAL_END_SEQ: a.flags |= F_END_OF_SEQ; break;
        case NAL_END_STREAM: a.flags |= F_END_OF_STREAM; break;
        case NAL_FILLER: a.flags |= F_FILLER; break;
        case NAL_SEI: {
            Rbsp b = payload_of(n);
            for (SeiMessage& m : parse_sei(b.data())) a.sei.push_back(std::move(m));
            break;
        }
        case NAL_SLICE:
        case NAL_IDR:
            if (!a.first) {
                Rbsp b = payload_of(n);
                const Sps* s = nullptr;
                const Pps* p = nullptr;
                SliceHeader h = parse_slice_header(b, n.type == NAL_IDR, n.ref_idc, spss.data(), ppss.data(), &s, &p);
                if (h.valid) {
                    a.first = h;
                    a.first_nal = &n;
                    a.first_sps = s;
                    a.first_pps = p;
                }
            }
            break;
        default: break;
        }
    }
    for (const SeiMessage& m : a.sei) {
        if (m.type == 0) a.flags |= F_BUFFERING_SEI;
        if (m.type == 1) a.flags |= F_PIC_TIMING_SEI_0;
        if (m.type == 5) a.flags |= F_USER_DATA_SEI;
    }
}

// What Sony's core makes of a sequence when a picture activates it: 0 it decodes it, else the error code it gives
// (0x10 a profile or level it does not know, 0x12 one it does not decode: Extended, High 10, High 4:2:2, chroma other
// than 4:2:0, more than 8 bits, a frame under 64 x 64, or every sequence after Reset with flags 0; 0x13 a frame larger
// than the decoder was opened for), and whether its access units still give (skipped, in error) pictures: not for
// High 4:4:4 and CAVLC 4:4:4, whose SPS its parser reads as another profile's (all measured).
struct SeqCheck {
    u8 code = 0;
    bool pictures = true;
};

SeqCheck check_sequence(const Decoder& d, const Sps& s)
{
    const u8 pr = s.profile_idc;
    if (pr == 44 || pr == 244) return {0x10, false};
    if (pr != 66 && pr != 77 && pr != 100 && pr != 88 && pr != 110 && pr != 122) return {0x10, true};
    constexpr std::array<u8, 16> kKnownLevels{10, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52};
    if (std::find(kKnownLevels.begin(), kKnownLevels.end(), s.level_idc) == kKnownLevels.end()) return {0x10, true};
    if (d.reset_flags == 0 || pr == 88 || pr == 110 || pr == 122) return {0x12, true};
    if (s.chroma_format_idc != 1 || s.bit_depth_luma != 8 || s.bit_depth_chroma != 8) return {0x12, true};
    if (s.width_mbs < 4 || s.frame_height_mbs() < 4) return {0x12, true};
    if (s.width_mbs * s.frame_height_mbs() > d.lay.frame_mbs) return {0x13, true};
    return {};
}

// The decoded picture buffer a sequence gets: its own size (dpb_size_of), less when the frame buffers the decoder was
// opened with hold fewer frames of its size than that and two more (measured: a 64 x 80 stream in a decoder opened for
// 5 x 4 macroblocks has 16 frame buffers, so a buffer of 14).
u32 effective_dpb(const Decoder& d, const Sps& s)
{
    const u32 want = dpb_size_of(s);
    // libmvcdec's frame buffers are two views' (with one view too)
    const u32 views = d.core->mvc ? 2 : 1;
    const u32 fit = d.lay.blocks[4] / frame_slot_bytes(s.width(), s.height()) / views;
    return fit >= want + 2 ? want : std::max<u32>(fit, 3) - 2;
}

// The access unit's parameter sets kept (an SPS: the next I picture starts a GOP).
void commit_params(Decoder& d, const AuContent& a)
{
    for (const Sps& s : a.sps)
        if (s.valid && s.id < 32) {
            d.sps[s.id] = s;
            d.sps_pending = true;
        }
    for (const Pps& p : a.pps)
        if (p.valid && p.id < 256) d.pps[p.id] = p;
}

// What kind of access unit Sony's core does not decode.
enum class Failed {
    NoSequence, // no slice it can decode and no sequence yet: a 64 x 64 picture, no sequence's fields
    InSequence, // no slice it can decode within a sequence: the active sequence's size and fields, the last picture's
                // type and order count
    Unknown,    // a slice of a sequence of a profile or level it does not know: 64 x 64, the slice's type and count
    Refused,    // a slice of a sequence it does not decode: the sequence's size and fields, the slice's type and count
};

// An access unit Sony's core does not decode: its AU done gives the error (and whether a sequence it decodes is active),
// then, unless `picture` is false, a picture comes out at once, skipped and in error, in a frame buffer of its own (the
// next in turn; the first, blank, when none are laid out yet). The decoded picture buffer stays as it was. False when it
// waits for a frame buffer (nothing changed).
bool error_au(u8* base, Decoder& d, const Command& c, const AuContent& a, Failed kind, const Sps* sps, u8 code,
    bool picture, std::vector<Message>& msgs)
{
    const bool valid = kind == Failed::InSequence;
    const bool own_size = kind == Failed::InSequence || kind == Failed::Refused;
    PicturePtr p;
    if (picture && d.core->mvc) {
        // libmvcdec's error pictures have no frame buffer, though one is passed over in turn (none by the second of two
        // in a row: measured)
        if (d.slots && !d.last_failed) {
            std::lock_guard lk(d.m);
            (void)take_slot(d);
        }
        d.last_failed = true;
        p = std::make_shared<Picture>();
        p->slot = kNoSlot;
        p->width = own_size ? sps->width() : 64;
        p->height = own_size ? sps->height() : 64;
    } else if (picture) {
        u32 slot = 0;
        if (d.slots) {
            std::lock_guard lk(d.m);
            const std::optional<u32> s = take_slot(d);
            if (!s) return false;
            slot = *s;
        }
        p = std::make_shared<Picture>();
        p->slot = slot;
        p->width = own_size ? sps->width() : 64;
        p->height = own_size ? sps->height() : 64;
        p->frame = d.slots ? slot_addr(d, slot) : d.mem + d.core->dynamic_at + d.lay.frames;
        // A frame buffer no decoded picture was in since the layout (or the first, before any layout) is made blank;
        // one that held a picture is left as it was (Sony's core at times conceals into it: a copy of a picture moved
        // by a line; not reproduced, D-933).
        if (!d.slots || slot >= d.slot_used.size() || !d.slot_used[slot]) fill_error_frame(base, p->frame, p->width, p->height);
    }
    commit_params(d, a);
    if (p) {
        FrameReport& r = p->report;
        r.flags = a.flags;
        if (own_size) sequence_fields(*sps, r, d.core->mvc);
        r.width = p->width;
        r.height = p->height;
        if (kind == Failed::InSequence) {
            r.type[0] = r.type[1] = d.last_type;
            r.poc[0] = d.last_poc;
        } else if (a.first) {
            const SliceHeader& first = *a.first;
            const bool idr = a.first_nal->type == NAL_IDR;
            if (idr && !(d.core->mvc && kind == Failed::Unknown)) r.flags |= F_IDR; // libmvcdec's lacks it there
            if (a.first_pps && a.first_pps->cabac) r.flags |= F_CABAC;
            r.type[0] = r.type[1] = static_cast<u8>(first.is_i() ? 0 : first.is_p() ? 1 : 2);
            r.poc[0] = picture_order_count(*a.first_sps, first, idr, a.first_nal->ref_idc, d.poc_state).top;
            if (kind == Failed::Refused && first.is_i()) {
                r.gop_boundary = d.sps_pending ? 1 : 0;
                d.sps_pending = false;
            }
        }
        r.dts[0] = c.dts;
        r.pts[0] = c.pts;
        r.field_interval = 0x7FFF;
        r.side[0] = c.side_id;
        r.tail = {4, 4, 4, 4, 4, 4};
        r.skipped = 1;
        r.error = 1;
        r.error_code = code;
    }
    Message m;
    m.type = MSG_AU_DONE;
    m.au.addr = c.addr;
    m.au.cont = c.cont;
    m.au.size = c.size;
    m.au.cont_size = c.cont_size;
    m.au.dep = c.dep;
    m.au.dep_size = c.dep_size;
    m.au.dep_cont = c.dep_cont;
    m.au.dep_cont_size = c.dep_cont_size;
    m.au.pictures = d.core->mvc && d.views == 2 ? 2 : 1;
    m.au.side_id = c.side_id;
    m.au.error = 1;
    m.au.error_code = code;
    m.au.bd_decodable = valid ? 1 : 0;
    m.au.bd_cv = valid ? 0 : 1;
    msgs.push_back(m);
    if (p) {
        std::lock_guard lk(d.m);
        emit_output(d, p, msgs);
    }
    return true;
}

// libmvcdec's second view picture of an access unit: its own frame buffer, its report the base view picture's with the
// second view's NAL units' flags, its sequence's fields and its slice's type, view 1; its samples decoded from the
// second view (vdec_mvc_view.cpp), or the base view's when they cannot be.
void second_view_picture(u8* base, Decoder& d, const Command& c, const PicturePtr& p, u32 slot, bool skip,
    const BaseView& decoded)
{
    std::vector<u8> bytes;
    if (c.dep && c.dep_size) bytes.assign(base + c.dep, base + c.dep + c.dep_size);
    if (c.dep_cont && c.dep_cont_size) bytes.insert(bytes.end(), base + c.dep_cont, base + c.dep_cont + c.dep_cont_size);
    auto q = std::make_shared<Picture>();
    q->slot = slot;
    q->frame = slot == kNoSlot ? 0 : slot_addr(d, slot);
    if (slot < d.slot_used.size()) d.slot_used[slot] = true;
    q->width = p->width;
    q->height = p->height;
    q->poc = p->poc;
    q->first = p;
    FrameReport& r = q->report;
    r = p->report;
    r.view = 1;
    r.first_of_unit = 0;
    r.flags = 0;
    const std::vector<Nal> nals = split_nals(bytes);
    const Sps* sps = nullptr;
    for (const Nal& n : nals) {
        switch (n.type) {
        case NAL_SUBSET_SPS: {
            Rbsp b = payload_of(n);
            Sps s = parse_sps(b, true);
            if (s.valid && s.id < 32) d.subset[s.id] = s;
            r.flags |= F_SPS;
            break;
        }
        case NAL_PPS: r.flags |= F_PPS; break;
        case NAL_AUD: r.flags |= F_AUD; break;
        case NAL_END_SEQ: r.flags |= F_END_OF_SEQ; break;
        case NAL_END_STREAM: r.flags |= F_END_OF_STREAM; break;
        case NAL_FILLER: r.flags |= F_FILLER; break;
        case NAL_SLICE_EXT:
            if (!sps) {
                Rbsp b = payload_of(n);
                b.ue();
                const u32 type = b.ue() % 5;
                r.type[0] = r.type[1] = static_cast<u8>(type == SLICE_I || type == SLICE_SI ? 0 : type == SLICE_B ? 2 : 1);
                for (const auto& s : d.subset)
                    if (s) sps = &*s;
            }
            break;
        default: break;
        }
    }
    if (sps) sequence_fields(*sps, r, true);
    else r.flags |= p->report.flags & (F_FRAME_MBS_ONLY | (3u << F_CHROMA_FORMAT_SHIFT));
    p->second = q;
    if (skip || decoded.y.empty()) return;
    ViewSamples out;
    if (d.second_view.decode(bytes, decoded, out))
        write_frame(base, q->frame, q->width, q->height, out.y.data(), out.u.data(), out.v.data());
    else
        write_frame(base, q->frame, q->width, q->height, decoded.y.data(), decoded.u.data(), decoded.v.data());
}

// libmvcdec's decoded picture buffer (its SPU side's program, not libavcdec's): a picture is stored as soon as it is
// decoded, as H.264's reference decoder (JM) does: an IDR picture outputs (or drops) what is there, a reference
// picture's marking comes first, then pictures are output while the buffer is full, a non-reference picture that would
// come first being output at once instead of stored (measured: a buffer of one outputs the picture before when the
// next is decoded, a non-reference picture at once).
// Its marking first (a reference picture's sliding window or memory management operations; an IDR picture emptied the
// buffer before), then the store.
void mark_now(Decoder& d, const Sps& sps, const PicturePtr& p)
{
    if (p->idr || !p->reference) return;
    update_wraps(d, sps, p->frame_num);
    if (p->adaptive) adaptive_marking(d, p);
    else sliding_window(d, sps);
}

void store_now(Decoder& d, const Sps& sps, const PicturePtr& p, std::vector<PicturePtr>& out)
{
    d.dpb_size = std::max<u32>(1, effective_dpb(d, sps));
    while (d.dpb.size() >= d.dpb_size) {
        const PicturePtr first = smallest_poc(d);
        if (!p->reference && (!first || p->poc < first->poc)) {
            p->output_done = true;
            out.push_back(p);
            return;
        }
        if (!first) break;
        output(d, first, out);
    }
    p->in_dpb = true;
    d.dpb.push_back(p);
}

// The SPU side's work on one access unit: false when it waits for a frame buffer (nothing of the decoder changed).
bool decode_au(PPUContext& ctx, Decoder& d, const Command& c, std::vector<Message>& msgs)
{
    u8* base = guest_base(ctx);
    take_releases(d);
    AuContent a;
    read_au(base, d, c, a);
    AuReport au;
    au.addr = c.addr;
    au.cont = c.cont;
    au.side_id = c.side_id;
    au.size = c.size;
    au.cont_size = c.cont_size;
    au.dep = c.dep;
    au.dep_size = c.dep_size;
    au.dep_cont = c.dep_cont;
    au.dep_cont_size = c.dep_cont_size;
    const bool two_views = d.core->mvc && d.views == 2;
    au.pictures = two_views ? 2 : 1;
    if (!a.first) {
        // No slice it decodes (none, one cut short, or one whose parameter sets are missing): with a sequence active
        // 0x50 (a slice there) or 0x51 (none); before any, 0x10 or 0x11 for the first such access unit, 0 for the
        // next ones. Its parameter sets are kept; a picture in error comes out, of the active sequence's size, else
        // of the access unit's SPS's, else 64 x 64.
        bool slice_nal = false;
        for (const Nal& n : a.nals)
            if (n.type == NAL_SLICE || n.type == NAL_IDR) slice_nal = true;
        PS_LOG_WARN(d.core->library, "decoder {:#x}: an access unit with no slice to decode ({} bytes)", d.handle, a.data.size());
        u8 code = 0;
        if (d.seq_valid) code = slice_nal ? 0x50 : 0x51;
        else if (!d.error_reported) code = slice_nal ? 0x10 : 0x11;
        const bool in_sequence = d.seq_valid && d.active;
        if (!error_au(base, d, c, a, in_sequence ? Failed::InSequence : Failed::NoSequence, in_sequence ? &*d.active : nullptr,
                code, true, msgs))
            return false;
        if (!d.seq_valid) d.error_reported = true;
        return true;
    }
    const Sps sps = *a.first_sps;
    const Pps pps = *a.first_pps;
    const SliceHeader& first = *a.first;
    const bool idr = a.first_nal->type == NAL_IDR;
    const u32 ref_idc = a.first_nal->ref_idc;
    const bool reference = ref_idc != 0;
    // A sequence the core refuses: an error picture for each access unit, the code with the first only.
    if (const SeqCheck chk = check_sequence(d, sps); chk.code) {
        PS_LOG_WARN(d.core->library, "decoder {:#x}: a sequence it does not decode (profile {}, level {}, {}x{}): error {:#x}", d.handle,
            sps.profile_idc, sps.level_idc, sps.width(), sps.height(), chk.code);
        const Failed kind = chk.code == 0x10 ? Failed::Unknown : Failed::Refused;
        if (!error_au(base, d, c, a, kind, &sps, d.error_reported ? 0 : chk.code, chk.pictures, msgs)) return false;
        d.error_reported = true;
        d.seq_valid = false;
        return true;
    }
    // The buffer before this picture (once: an access unit that waits for a frame buffer does it no more): the picture
    // decoded last joins it; an IDR picture empties it (its pictures output, unless no_output_of_prior_pics_flag), a
    // reference picture's marking (the sliding window, or its memory management operations) comes next; then pictures
    // are output until the buffer holds no more than its size, and those whose PTS this access unit's DTS reached. The
    // outputs wait for this access unit's AUDONE.
    std::vector<bool> was_free;
    if (!d.core->mvc && !d.previous_stored) {
        {
            std::lock_guard lk(d.m);
            for (u32 k = 0; k < d.slots; k++) was_free.push_back(!slot_busy(d, k));
        }
        d.previous_stored = true;
        d.queued.clear();
        if (d.current) {
            d.current->in_dpb = true;
            d.dpb.push_back(d.current);
            d.current.reset();
        }
        if (idr) {
            if (first.no_output_of_prior_pics) {
                for (const PicturePtr& q : d.dpb) q->in_dpb = false;
                d.dpb.clear();
            } else {
                flush_dpb(d, d.queued);
            }
        } else if (reference) {
            update_wraps(d, sps, first.frame_num);
            if (first.adaptive) {
                Picture marking;
                marking.frame_num = first.frame_num;
                marking.mmco = first.mmco;
                adaptive_marking(d, std::make_shared<Picture>(marking));
            } else {
                sliding_window(d, sps);
            }
        }
        d.dpb_size = effective_dpb(d, sps);
        bump(d, d.queued);
        if (c.dts != kNoTime) output_by_time(d, c.dts, d.queued);
        std::lock_guard lk(d.m);
        free_ids(d);
    }
    // The frame buffers, laid out again for a new frame size or buffer; the picture's.
    const u32 dpb = effective_dpb(d, sps);
    const bool relayout = d.slots == 0 || d.frame_width != sps.width() || d.frame_height != sps.height() || d.dpb_size != dpb;
    // The decode modes 1 to 3 leave non-reference pictures out (as B pictures are skipped); libmvcdec gives them no
    // frame buffer.
    const bool skip = c.mode != 0 && !reference;
    const bool no_buffer = d.core->mvc && skip;
    std::optional<u32> slot, slot2;
    if (no_buffer) {
        slot = kNoSlot;
        if (two_views) slot2 = kNoSlot;
    } else if (relayout) {
        slot = 1;
        if (two_views) slot2 = 2;
    } else {
        std::lock_guard lk(d.m);
        const u32 last = d.last_slot;
        slot = take_slot(d, was_free);
        if (slot && two_views) {
            slot2 = take_slot(d, was_free);
            if (!slot2) {
                slot.reset();
                d.last_slot = last;
            }
        }
    }
    if (!slot) return false;
    // From here on the access unit is decoded.
    d.previous_stored = false;
    if (two_views && idr) {
        // libmvcdec with two views: an IDR picture outputs (or drops) what is in the buffer before its access unit is
        // done (with one view, after).
        std::vector<PicturePtr> out;
        if (first.no_output_of_prior_pics) {
            for (const PicturePtr& q : d.dpb) q->in_dpb = false;
            d.dpb.clear();
        } else {
            flush_dpb(d, out);
        }
        std::lock_guard lk(d.m);
        for (const PicturePtr& q : out) emit_output(d, q, msgs);
    }
    if (relayout && !no_buffer) {
        lay_out_frames(d, sps.width(), sps.height(), dpb);
        d.last_slot = two_views ? 2 : 1;
    }
    d.dpb_size = dpb;
    d.seq_valid = true;
    d.error_reported = false;
    d.last_failed = false;
    commit_params(d, a);
    auto p = std::make_shared<Picture>();
    p->slot = *slot;
    p->frame = *slot == kNoSlot ? 0 : slot_addr(d, *slot);
    if (*slot < d.slot_used.size()) d.slot_used[*slot] = true;
    p->width = sps.width();
    p->height = sps.height();
    p->idr = idr;
    p->reference = reference;
    p->frame_num = first.frame_num;
    p->no_output_of_prior_pics = first.no_output_of_prior_pics;
    p->adaptive = first.adaptive;
    p->mmco = first.mmco;
    p->long_term = idr && first.long_term_reference;
    const Poc poc = picture_order_count(sps, first, idr, ref_idc, d.poc_state);
    p->poc = poc.frame();
    FrameReport& r = p->report;
    r.flags = a.flags;
    sequence_fields(sps, r, d.core->mvc);
    if (idr) r.flags |= F_IDR;
    if (pps.cabac) r.flags |= F_CABAC;
    r.dts[0] = c.dts;
    r.pts[0] = c.pts;
    if (c.dts != kNoTime && c.pts != kNoTime && c.pts >= c.dts) p->out_time = c.pts;
    r.poc[0] = poc.top;
    r.poc[1] = 0;
    r.field_interval = 0x7FFF;
    r.side[0] = c.side_id;
    r.type[0] = r.type[1] = static_cast<u8>(first.is_i() ? 0 : first.is_p() ? 1 : 2);
    d.last_type = r.type[0];
    d.last_poc = r.poc[0];
    // An I picture after an SPS (in its access unit or one before) starts a GOP (measured: an IDR picture without one
    // does not; a P picture with one leaves it to the next I picture).
    if (first.is_i()) {
        r.gop_boundary = d.sps_pending ? 1 : 0;
        d.sps_pending = false;
    }
    r.tail = {4, 4, 4, 4, 4, 4};
    if (skip) r.skipped = 1;
    if (two_views) r.pictures = 2;
    BaseView decoded;
#if PSPOCKET_HAVE_FFMPEG
    if (!d.samples.is_open() && !d.samples.open())
        PS_LOG_ERROR(d.core->library, "decoder {:#x}: FFmpeg's H.264 decoder did not open", d.handle);
    bool damaged = false;
    const bool got = d.samples.decode(a.data.data(), static_cast<u32>(a.data.size()), skip, damaged);
    if (got && !skip && d.samples.planes(p->width, p->height, decoded.y, decoded.u, decoded.v)) {
        write_frame(base, p->frame, p->width, p->height, decoded.y.data(), decoded.u.data(), decoded.v.data());
        decoded.width = p->width;
        decoded.height = p->height;
        decoded.poc = p->poc;
    }
#else
    PS_LOG_ERROR(d.core->library, "decoder {:#x}: pspocket was built without FFmpeg: nothing is decoded", d.handle);
#endif
    if (two_views) second_view_picture(base, d, c, p, *slot2, skip, decoded);
    d.active = sps;
    const auto emit_pending = [&] {
        if (!d.pending_second) return;
        emit_output(d, d.pending_second, msgs);
        d.pending_second.reset();
    };
    std::vector<PicturePtr> idr_out;
    if (d.core->mvc) {
        // libmvcdec: the picture's marking (with one view an IDR picture empties the buffer now, its outputs given
        // after the access unit is done), then (two views) the picture output at once before (its frame ID taken
        // now), before the access unit is done.
        if (!two_views && idr) {
            if (first.no_output_of_prior_pics) {
                for (const PicturePtr& q : d.dpb) q->in_dpb = false;
                d.dpb.clear();
            } else {
                flush_dpb(d, idr_out);
            }
        }
        mark_now(d, sps, p);
        std::lock_guard lk(d.m);
        free_ids(d);
        if (two_views) emit_pending();
    }
    // The access unit is done; then the pictures the store output.
    {
        Message m;
        m.type = MSG_AU_DONE;
        m.au = au;
        m.au.skipped = skip ? 1 : 0;
        msgs.push_back(m);
    }
    if (d.core->mvc) {
        // libmvcdec stores the picture now. With one view the picture output at once before comes first, and an IDR
        // picture's outputs next. A picture output at once has its message (two views: its second view picture's)
        // with the next access unit's; a skipped one's comes at once.
        std::vector<PicturePtr> out = idr_out;
        // the pictures whose PTS this access unit's DTS reached leave by the clock (as libavcdec's)
        if (c.dts != kNoTime) output_by_time(d, c.dts, out);
        store_now(d, sps, p, out);
        std::lock_guard lk(d.m);
        if (!two_views) emit_pending();
        for (const PicturePtr& q : out) {
            if (q == p && !skip) {
                if (two_views) emit_output(d, q, msgs, false);
                d.pending_second = two_views ? p->second : p;
                continue;
            }
            emit_output(d, q, msgs);
        }
        return true;
    }
    d.current = p;
    for (const PicturePtr& q : d.queued) {
        std::lock_guard lk(d.m);
        emit_output(d, q, msgs);
    }
    d.queued.clear();
    return true;
}

// The flush: the picture decoded last joins the buffer, and every picture is output.
void flush(Decoder& d, std::vector<Message>& msgs)
{
    std::vector<PicturePtr> out;
    if (d.core->mvc) {
        // libmvcdec: the IDs of released pictures that left the buffer since are free for the flush's outputs
        std::lock_guard lk(d.m);
        free_ids(d);
    }
    if (d.pending_second) {
        std::lock_guard lk(d.m);
        emit_output(d, d.pending_second, msgs);
        d.pending_second.reset();
    }
    if (d.current) {
        d.current->in_dpb = true;
        d.dpb.push_back(d.current);
        d.current.reset();
    }
    flush_dpb(d, out);
    for (const PicturePtr& q : out) {
        std::lock_guard lk(d.m);
        emit_output(d, q, msgs);
    }
    Message m;
    m.type = MSG_FLUSH_DONE;
    msgs.push_back(m);
}

// ---------------------------------------------------------------------------------------------------- messages

// A side data entry's place in the context, as the messages point at it.
u32 side_entry_addr(const Decoder& d, u32 index) { return d.handle + d.core->side_at + d.core->side_entry * index; }

int side_index(const Decoder& d, u32 key)
{
    for (u32 i = 0; i < d.core->side_entries; i++)
        if (d.side[i].used && d.side[i].key == key) return static_cast<int>(i);
    return -1;
}

// The PPU side's handling of the SPU side's answers, then the callback (on the helper thread, no lock held).
void deliver(PPUContext& ctx, Decoder& d, const Message& msg)
{
    u8* base = guest_base(ctx);
    // The message and the picture's information on the helper thread's stack, with room past them (a callback may
    // read a little more than the information).
    StackVar var(ctx, base, 0x300);
    const u32 at = var.addr();
    switch (msg.type) {
    case MSG_AU_DONE: {
        const AuReport& a = msg.au;
        u32 side_ptr = 0, side_size = 0;
        {
            std::lock_guard lk(d.m);
            if (d.in_flight) d.in_flight--;
            if (d.closing) return;
            const int i = side_index(d, a.side_id);
            if (i >= 0) {
                side_ptr = side_entry_addr(d, static_cast<u32>(i)) + 4;
                side_size = d.side[static_cast<usize>(i)].size;
            } else {
                PS_LOG_WARN(d.core->library, "decoder {:#x}: side data tag {} not in its table", d.handle, a.side_id);
            }
        }
        if (d.core->mvc) {
            // libmvcdec's: every piece with its size, then the same flags spread out, and the pictures the access
            // unit gives (its side data is freed once they are all released).
            {
                std::lock_guard lk(d.m);
                if (const int i = side_index(d, a.side_id); i >= 0) {
                    d.side[static_cast<usize>(i)].expected = a.pictures;
                    d.side[static_cast<usize>(i)].done = true;
                }
            }
            for (u32 o = 0; o < 0x44; o += 4) ps_write32(base, at + o, 0);
            ps_write32(base, at + 0x00, MSG_AU_DONE);
            ps_write32(base, at + 0x04, d.cb_arg);
            ps_write32(base, at + 0x08, a.addr);
            ps_write32(base, at + 0x0C, a.size);
            ps_write32(base, at + 0x10, a.cont);
            ps_write32(base, at + 0x14, a.cont_size);
            ps_write32(base, at + 0x18, a.dep);
            ps_write32(base, at + 0x1C, a.dep_size);
            ps_write32(base, at + 0x20, a.dep_cont);
            ps_write32(base, at + 0x24, a.dep_cont_size);
            ps_write32(base, at + 0x28, side_ptr);
            ps_write32(base, at + 0x2C, side_size);
            ps_write8(base, at + 0x30, a.discarded);
            ps_write32(base, at + 0x34, a.error);
            ps_write8(base, at + 0x38, a.bd_decodable);
            ps_write8(base, at + 0x39, a.bd_cv);
            ps_write32(base, at + 0x3C, external_error(a.error_code));
            ps_write8(base, at + 0x40, a.pictures);
        } else {
            ps_write32(base, at + 0x00, MSG_AU_DONE);
            ps_write32(base, at + 0x04, d.cb_arg);
            ps_write32(base, at + 0x08, a.addr);
            ps_write32(base, at + 0x0C, a.cont);
            ps_write32(base, at + 0x10, side_ptr);
            ps_write32(base, at + 0x14, side_size);
            ps_write8(base, at + 0x18, a.discarded);
            ps_write8(base, at + 0x19, a.error);
            ps_write8(base, at + 0x1A, a.bd_decodable);
            ps_write8(base, at + 0x1B, a.bd_cv);
            ps_write32(base, at + 0x1C, external_error(a.error_code));
        }
        if (d.callback) call_guest(ctx, d.callback, {at});
        if (a.discarded) {
            std::lock_guard lk(d.m);
            if (const int i = side_index(d, a.side_id); i >= 0) d.side[static_cast<usize>(i)].used = false;
        }
        return;
    }
    case MSG_FRAME: {
        const FrameReport& r = msg.frame;
        const u32 info = at + (d.core->mvc ? 0x44 : 0x24);
        u32 side_ptr[2] = {0, 0}, side_size[2] = {0, 0};
        u32 frame_id = r.frame_id;
        const u8 count = (r.structure > 2 && r.structure <= 6 && !r.decode_pair) ? 2 : 1;
        {
            std::lock_guard lk(d.m);
            if (d.state == ST_RESETTING || d.closing) return;
            if (r.discard) {
                for (u32 k = 0; k < count; k++)
                    if (const int i = side_index(d, r.side[k]); i >= 0) d.side[static_cast<usize>(i)].used = false;
                return;
            }
            if (r.skipped && !r.error) frame_id = 256u + d.skip_counter++;
            for (u32 k = 0; k < count; k++) {
                const int i = side_index(d, r.side[k]);
                if (i < 0) {
                    PS_LOG_WARN(d.core->library, "decoder {:#x}: side data tag {} (field {}) not in its table", d.handle, r.side[k], k);
                    continue;
                }
                side_ptr[k] = side_entry_addr(d, static_cast<u32>(i)) + 4;
                side_size[k] = d.side[static_cast<usize>(i)].size;
                if (d.core->mvc && k == 0) d.side[static_cast<usize>(i)].expected = r.pictures;
            }
            // The frame table: what the frame's release frees.
            bool added = false;
            for (u32 f = 0; f < d.core->frame_entries; f++)
                if (d.frames[f].used && d.frames[f].key == frame_id) added = true; // a key twice: refused (logged)
            if (!added) {
                for (u32 f = 0; f < d.core->frame_entries; f++)
                    if (auto& e = d.frames[f]; !e.used) {
                        e.used = true;
                        e.key = frame_id;
                        e.count = count;
                        e.side[0] = r.side[0];
                        e.side[1] = r.side[1];
                        e.skipped = r.skipped && !r.error ? 1 : 0;
                        added = true;
                        break;
                    }
                if (!added) PS_LOG_WARN(d.core->library, "decoder {:#x}: its frame table is full", d.handle);
            } else {
                PS_LOG_WARN(d.core->library, "decoder {:#x}: frame {} given again before its release", d.handle, frame_id);
            }
        }
        if (d.core->mvc) write_mvc_info(base, info, r, frame_id, count, side_ptr, side_size, r.view, r.first_of_unit, r.pictures);
        else write_info(base, info, r, frame_id, count, side_ptr, side_size);
        ps_write32(base, at + 0x00, MSG_FRAME);
        ps_write32(base, at + 0x04, d.cb_arg);
        ps_write32(base, at + 0x08, info);
        if (d.callback) call_guest(ctx, d.callback, {at});
        return;
    }
    case MSG_FLUSH_DONE:
    case MSG_RESET_DONE:
    case MSG_SEVERE: {
        {
            std::lock_guard lk(d.m);
            if (msg.type == MSG_FLUSH_DONE) d.state = ST_FLUSHED;
            if (msg.type == MSG_RESET_DONE) {
                d.state = ST_READY;
                for (auto& e : d.side) e.used = false;
                for (auto& e : d.frames) e.used = false;
            }
            if (msg.type == MSG_SEVERE) d.state = ST_FAILED;
        }
        ps_write32(base, at + 0x00, msg.type);
        ps_write32(base, at + 0x04, d.cb_arg);
        if (d.callback) call_guest(ctx, d.callback, {at});
        return;
    }
    default: return;
    }
}

// The decoder's reset (Reset's command): every picture dropped, the frame IDs and buffers free again.
void reset_decoder(Decoder& d, const Command& c)
{
    for (const PicturePtr& q : d.dpb) q->in_dpb = false;
    d.dpb.clear();
    d.current.reset();
    d.pending_second.reset();
    d.stalled.reset();
    d.previous_stored = false;
    d.queued.clear();
    {
        std::lock_guard lk(d.m);
        d.outputs.clear();
    }
    d.poc_state = {};
    d.last_slot = 0;
    d.reset_flags = c.reset_flags;
    d.seq_valid = false;
    d.error_reported = false;
    if (!c.keep_sps) {
        d.sps_pending = false;
        for (auto& s : d.sps) s.reset();
        for (auto& p : d.pps) p.reset();
        d.active.reset();
        d.slots = 0;
    }
#if PSPOCKET_HAVE_FFMPEG
    d.samples.reset();
#endif
    d.second_view.reset();
    if (!c.keep_sps)
        for (auto& s : d.subset) s.reset();
}

// The helper thread: the SPU side's work in command order, then the messages.
void run(PPUContext& ctx, Decoder& d)
{
    lv2::PpuThread& t = thread_of(ctx);
    for (;;) {
        Command c;
        bool from_stall = false;
        {
            std::unique_lock lk(d.m);
            park_until(t, d, lk, [&] {
                for (const Command& o : d.other_queue)
                    if (o.kind != Command::Kind::Release || d.stalled) return true;
                return !d.stalled && !d.au_queue.empty();
            });
            // Reset and close first (they are the other queue's commands besides the releases).
            auto it = std::find_if(d.other_queue.begin(), d.other_queue.end(),
                [](const Command& o) { return o.kind != Command::Kind::Release; });
            if (it != d.other_queue.end()) {
                c = *it;
                d.other_queue.erase(it);
            } else if (d.stalled) {
                c = *d.stalled;
                from_stall = true;
            } else {
                c = d.au_queue.front();
                d.au_queue.pop_front();
            }
        }
        std::vector<Message> msgs;
        switch (c.kind) {
        case Command::Kind::Close: {
            std::lock_guard lk(d.m);
            d.closing = false;
            d.shutting_down = false;
            return;
        }
        case Command::Kind::Reset:
            reset_decoder(d, c);
            msgs.push_back({MSG_RESET_DONE, {}, {}});
            break;
        case Command::Kind::Au:
            if (decode_au(ctx, d, c, msgs)) {
                d.stalled.reset();
            } else {
                d.stalled = c;
                (void)from_stall;
            }
            break;
        case Command::Kind::Flush: flush(d, msgs); break;
        case Command::Kind::Release: break;
        }
        for (const Message& m : msgs) deliver(ctx, d, m);
    }
}

// =================================================================================================== the API

// The decoder at `handle` when its memory holds the core's open mark (Sony's check).
std::shared_ptr<Decoder> decoder_of(PPUContext& ctx, const Core& core, u32 handle)
{
    if (!handle || !guest_readable(thread_of(ctx), handle, 4)) return nullptr;
    if (ps_read32(guest_base(ctx), handle) != core.magic) return nullptr;
    std::shared_ptr<Decoder> d = decoder_at(handle);
    return d && d->core == &core ? d : nullptr;
}

std::optional<Layout> core_layout(const Core& core, u32 level, u32 buffers, u32 width_mbs, u32 height_mbs)
{
    return core.mvc ? mvc_layout(level, buffers, width_mbs, height_mbs) : layout(level, buffers, width_mbs, height_mbs);
}

// QueryMemory(param, attr): the memory for the parameters, to 128 bytes, and its alignment (128).
s32 query_memory(PPUContext& ctx, const Core& core, u32 param, u32 attr)
{
    u8* base = guest_base(ctx);
    if (!writable(ctx, param, 8) || ps_read8(base, param + 1) > 2 || !writable(ctx, attr, 12)) {
        PS_LOG_ERROR(core.library, "QueryMemory({:#x}, {:#x}): bad parameter", param, attr);
        return core.arg();
    }
    const std::optional<Layout> l = core_layout(core, ps_read8(base, param), ps_read8(base, param + 2),
        ps_read16(base, param + 4), ps_read16(base, param + 6));
    if (!l) return core.arg();
    ps_write32(base, attr + 4, l->size);
    ps_write32(base, attr + 8, 0x80);
    return lv2::CELL_OK;
}

// Open's arguments (libavcdec's and libmvcdec's in their order, past eight in the caller's parameter save area).
struct OpenArgs {
    u32 handle_out = 0, spurs = 0, taskset_out = 0, param = 0, memdesc = 0, stack = 0, sdk = 0, callback = 0, cb_arg = 0;
    u32 nspus = 0, prios = 0;
    s32 spu_prio = 0, ppu_prio = 0;
};

s32 open_decoder(PPUContext& ctx, const Core& core, const OpenArgs& o)
{
    u8* base = guest_base(ctx);
    const bool params_ok = writable(ctx, o.param, 8) && ps_read8(base, o.param) <= 42 && ps_read8(base, o.param + 1) <= 2;
    const bool mem_ok = writable(ctx, o.memdesc, 12) && ps_read32(base, o.memdesc) && !(ps_read32(base, o.memdesc) & 0x7F);
    if (!o.handle_out || !writable(ctx, o.handle_out, 4) || (o.spurs && !o.prios) || !params_ok || !o.callback ||
        !mem_ok || o.stack < 0x1000 || o.nspus == 0 || o.nspus > 8 || o.spu_prio < 0 || o.spu_prio > 0xFF ||
        o.ppu_prio < 0 || o.ppu_prio > 0xBFF) {
        PS_LOG_ERROR(core.library, "Open: bad parameter");
        return core.arg();
    }
    const u32 level = ps_read8(base, o.param), deblock = ps_read8(base, o.param + 1), buffers = ps_read8(base, o.param + 2);
    const u32 wmbs = ps_read16(base, o.param + 4), hmbs = ps_read16(base, o.param + 6);
    const u32 version = core.mvc ? 0x10C00 : sdk_level(o.sdk);
    const std::optional<Layout> l = core_layout(core, level, buffers, wmbs, hmbs);
    if (!l) {
        if (version >= 0x10C00) return core.arg();
        PS_LOG_ERROR(core.library, "Open: parameters the memory query refuses, from an SDK before 3.3 (Sony's module goes "
                                   "on with sizes it never set): refused");
        return core.arg();
    }
    const u32 mem = ps_read32(base, o.memdesc), mem_size = ps_read32(base, o.memdesc + 4),
              align = ps_read32(base, o.memdesc + 8);
    if (mem_size < l->size) {
        PS_LOG_ERROR(core.library, "Open: memory is too small ({:#x} < {:#x})", mem_size, l->size);
        return core.arg();
    }
    if (align < 0x80) {
        PS_LOG_ERROR(core.library, "Open: memory alignment is incorrect ({:#x})", align);
        return core.arg();
    }
    if (!writable(ctx, mem, l->size)) {
        PS_LOG_ERROR(core.library, "Open: the memory ({:#x}, {:#x} bytes) is not guest memory", mem, l->size);
        return core.arg();
    }
    // The fixed part cleared, as Sony's (its task contexts and queues).
    std::memset(base + mem, 0, core.spurs_at);
    auto d = std::make_shared<Decoder>();
    d->core = &core;
    d->handle = mem + l->size - core.context_bytes;
    d->mem = mem;
    d->mem_size = l->size;
    d->lay = *l;
    d->level = level;
    d->deblock = deblock;
    d->buffers = buffers;
    d->width_mbs = wmbs;
    d->height_mbs = hmbs;
    d->sdk = version;
    d->callback = o.callback;
    d->cb_arg = o.cb_arg;
    // SPURS: the caller's, or the core's own in its memory.
    if (!o.spurs) {
        StackVar attr(ctx, base, 0x200);
        s32 r = static_cast<s32>(call_import(ctx, "cellSpurs", NID_cellSpursAttributeInitialize,
            {attr.addr(), 2, 0x330000, o.nspus, static_cast<u64>(o.spu_prio), static_cast<u64>(o.ppu_prio), 1}));
        if (r != lv2::CELL_OK) return r;
        StackVar name(ctx, base, 16);
        std::memcpy(base + name.addr(), core.prefix, std::strlen(core.prefix) + 1);
        r = static_cast<s32>(call_import(ctx, "cellSpurs", NID_cellSpursAttributeSetNamePrefix, {attr.addr(), name.addr(), 12}));
        if (r != lv2::CELL_OK) return r;
        r = static_cast<s32>(
            call_import(ctx, "cellSpurs", NID_cellSpursInitializeWithAttribute, {mem + core.spurs_at, attr.addr()}));
        if (r != lv2::CELL_OK) return r;
        d->own_spurs = true;
        d->spurs = mem + core.spurs_at;
    } else {
        d->spurs = o.spurs;
    }
    // The taskset, named after the handle, its priorities the caller's (or all ones for the core's own SPURS).
    {
        StackVar attr(ctx, base, 0x200);
        StackVar prio(ctx, base, 16);
        if (o.spurs) std::memcpy(base + prio.addr(), base + o.prios, 8);
        else std::memset(base + prio.addr(), 1, 8);
        s32 r = static_cast<s32>(call_import(ctx, "cellSpurs", NID_cellSpursTasksetAttributeInitialize,
            {attr.addr(), 1, 0x330000, d->handle + 0x80, prio.addr(), o.nspus}));
        if (r != lv2::CELL_OK) return r;
        StackVar name(ctx, base, 32);
        const std::string n = std::format("{}_{:08x}", core.prefix, d->handle);
        std::memcpy(base + name.addr(), n.c_str(), n.size() + 1);
        r = static_cast<s32>(call_import(ctx, "cellSpurs", NID_cellSpursTasksetAttributeSetName, {attr.addr(), name.addr()}));
        if (r != lv2::CELL_OK) return r;
        r = static_cast<s32>(call_import(ctx, "cellSpurs", NID_cellSpursTasksetAttributeSetTasksetSize, {attr.addr(), 0x2900}));
        if (r != lv2::CELL_OK) return r;
        r = static_cast<s32>(call_import(ctx, "cellSpurs", NID_cellSpursCreateTasksetWithAttribute,
            {d->spurs, mem + core.taskset_at, attr.addr()}));
        if (r != lv2::CELL_OK) return r;
        d->taskset = mem + core.taskset_at;
        if (o.taskset_out && writable(ctx, o.taskset_out, 4)) ps_write32(base, o.taskset_out, d->taskset);
    }
    {
        Decoders& all = decoders();
        std::lock_guard lk(all.m);
        all.by_handle[d->handle] = d;
    }
    // The helper thread.
    lv2::PpuThread& t = thread_of(ctx);
    HleProcess& p = *HleProcess::current();
    u32 entry = p.descriptor_of(std::string(core.library), core.helper_nid);
    if (!entry) entry = p.link_function(std::string(core.library), core.helper_nid, core.helper);
    d->tls = p.alloc_tls();
    StackVar sv(ctx, base, 0x40);
    ps_write32(base, sv.addr() + 8, entry);
    ps_write32(base, sv.addr() + 12, d->tls ? d->tls + kTlsR13Offset : 0);
    std::memcpy(base + sv.addr() + 0x10, core.prefix, std::strlen(core.prefix) + 1);
    s32 r = entry ? lv2::_sys_ppu_thread_create(t, sv.addr(), sv.addr() + 8, d->handle, 0, o.ppu_prio, o.stack, 1,
                        sv.addr() + 0x10)
                  : core.fatal();
    if (r == lv2::CELL_OK) r = lv2::sys_ppu_thread_start(t, static_cast<u32>(ps_read64(base, sv.addr())));
    if (r != lv2::CELL_OK) {
        PS_LOG_ERROR(core.library, "Open: the helper thread: {:#x}", static_cast<u32>(r));
        if (d->tls) p.free_tls(d->tls);
        Decoders& all = decoders();
        std::lock_guard lk(all.m);
        all.by_handle.erase(d->handle);
        return core.fatal();
    }
    d->thread_id = ps_read64(base, sv.addr());
    {
        std::lock_guard lk(d->m);
        d->seq += 2; // Open's command (and the helper thread's start)
        d->state = ST_READY;
    }
    ps_write32(base, d->handle, core.magic);
    ps_write32(base, o.handle_out, d->handle);
    return lv2::CELL_OK;
}

// The checks every call on a decoder makes: the handle (the open mark), then the state under the lock. A closed
// decoder's memory keeps its mark, but its critical section (an lv2 mutex) is destroyed: the calls that lock it get
// sys_mutex_lock's ESRCH.
constexpr u32 kLockGone = 0x80010005;
// libmvcdec maps its lock's failure through its own error codes, which have none for it: the answer is what its stack
// held there, 2 (measured).
s32 lock_gone(const Decoder& d) { return d.core->mvc ? 2 : error(kLockGone); }

s32 close_decoder(PPUContext& ctx, const Core& core, u32 handle)
{
    if (!handle) return core.arg();
    const std::shared_ptr<Decoder> d = decoder_of(ctx, core, handle);
    if (!d) return core.arg();
    lv2::PpuThread& t = thread_of(ctx);
    {
        std::lock_guard lk(d->m);
        if (d->state == ST_CLOSED || t.id == d->thread_id) return core.seq();
        if (d->state != ST_FAILED) d->seq += 2;
        d->state = ST_CLOSED;
        d->closing = true;
        Command c;
        c.kind = Command::Kind::Close;
        d->other_queue.push_back(c);
        wake(*d);
    }
    u8* base = guest_base(ctx);
    StackVar status(ctx, base, 8);
    lv2::sys_ppu_thread_join(t, static_cast<u32>(d->thread_id), status.addr());
    s32 r = static_cast<s32>(call_import(ctx, "cellSpurs", NID_cellSpursShutdownTaskset, {d->taskset}));
    if (r == lv2::CELL_OK) r = static_cast<s32>(call_import(ctx, "cellSpurs", NID_cellSpursJoinTaskset, {d->taskset}));
    if (r == lv2::CELL_OK && d->own_spurs)
        r = static_cast<s32>(call_import(ctx, "cellSpurs", NID_cellSpursFinalize, {d->spurs}));
    if (d->tls) HleProcess::current()->free_tls(d->tls);
    d->tls = 0;
    std::lock_guard lk(d->m);
    d->destroyed = true;
    return r;
}

s32 reset_call(PPUContext& ctx, const Core& core, u32 handle, u32 keep_sps, u32 flags)
{
    if (!handle || flags > 3) return core.arg();
    const std::shared_ptr<Decoder> d = decoder_of(ctx, core, handle);
    if (!d) return core.arg();
    std::lock_guard lk(d->m);
    if (d->destroyed) return lock_gone(*d);
    if (d->state == ST_FLUSHING || d->state == ST_RESETTING || d->state == ST_CLOSED) return core.seq();
    if (d->other_queue.size() + 2 > core.other_room) return core.busy();
    d->seq++;
    Command c;
    c.kind = Command::Kind::Reset;
    c.keep_sps = keep_sps & 0xFF;
    c.reset_flags = flags;
    d->other_queue.push_back(c);
    d->state = ST_RESETTING;
    wake(*d);
    return lv2::CELL_OK;
}

// An access unit's arguments: its pieces (libmvcdec's second view's too), its stamps, the decode mode, the side data.
struct AuArgs {
    u32 addr = 0, size = 0, cont = 0, cont_size = 0;
    u32 dep = 0, dep_size = 0, dep_cont = 0, dep_cont_size = 0;
    u64 dts = 0, pts = 0;
    u32 mode = 0, side = 0, side_size = 0;
};

s32 access_unit(PPUContext& ctx, const Core& core, u32 handle, const AuArgs& a)
{
    u8* base = guest_base(ctx);
    const std::shared_ptr<Decoder> d = decoder_of(ctx, core, handle);
    if (!d) return core.arg();
    if (core.mvc) {
        if (u64{a.size} + a.cont_size > 0x1FFFFF80 || u64{a.dep_size} + a.dep_cont_size > 0x1FFFFF80) return core.arg();
        // The first access unit fixes the views: one, or two (the second view's piece given); later ones must agree.
        if ((d->views == 2 && !a.dep_size) || (d->views == 1 && a.dep_size)) {
            PS_LOG_ERROR(core.library, "AccessUnit: the views differ from the first access unit's");
            return core.arg();
        }
    } else if (d->sdk > 0x105FF && (a.size > 0x1FFFFF80 || a.cont_size > 0x1FFFFF80 - a.size)) {
        return core.arg();
    }
    if (!writable(ctx, a.addr, a.size) || (a.cont_size && !writable(ctx, a.cont, a.cont_size)) ||
        (a.dep_size && !writable(ctx, a.dep, a.dep_size)) || (a.dep_cont_size && !writable(ctx, a.dep_cont, a.dep_cont_size)) ||
        (a.side_size && !writable(ctx, a.side, a.side_size))) {
        PS_LOG_ERROR(core.library, "AccessUnit: the access unit ({:#x}, {:#x} bytes) is not guest memory", a.addr, a.size);
        return core.arg();
    }
    std::lock_guard lk(d->m);
    if (d->destroyed) return lock_gone(*d);
    if (d->state != ST_READY && d->state != ST_DECODING) return core.seq();
    if (d->au_queue.size() + 2 > core.au_room) return core.busy();
    // The side data, under a tag: the next one free of the counter's.
    int slot = -1;
    for (u32 tries = 0; tries < 256; tries++) {
        bool taken = false;
        for (u32 i = 0; i < core.side_entries; i++)
            if (d->side[i].used && d->side[i].key == d->side_counter) taken = true;
        if (!taken) break;
        d->side_counter++;
    }
    for (u32 i = 0; i < core.side_entries; i++)
        if (!d->side[i].used) {
            slot = static_cast<int>(i);
            break;
        }
    if (slot < 0) {
        PS_LOG_ERROR(core.library, "AccessUnit: the side data table is full");
        return core.busy();
    }
    auto& e = d->side[static_cast<usize>(slot)];
    e = {};
    e.used = true;
    e.key = d->side_counter;
    e.size = a.side_size;
    const u32 entry = d->handle + core.side_at + core.side_entry * static_cast<u32>(slot);
    ps_write32(base, entry, a.side_size);
    for (u32 i = 0; i < 0x40; i++) ps_write8(base, entry + 4 + i, i < a.side_size ? ps_read8(base, a.side + i) : 0);
    if (core.mvc && !d->views) d->views = a.dep_size ? 2 : 1;
    Command c;
    c.kind = Command::Kind::Au;
    c.addr = a.addr;
    c.size = a.size;
    c.cont = a.cont;
    c.cont_size = a.cont_size;
    c.dep = a.dep;
    c.dep_size = a.dep_size;
    c.dep_cont = a.dep_cont;
    c.dep_cont_size = a.dep_cont_size;
    c.dts = a.dts;
    c.pts = a.pts;
    c.mode = a.mode;
    c.side_id = d->side_counter;
    d->seq++;
    d->au_queue.push_back(c);
    d->state = ST_DECODING;
    d->side_counter++;
    d->in_flight++;
    wake(*d);
    return lv2::CELL_OK;
}

// A side data entry's release (libmvcdec): freed once every picture of its access unit was released and the access unit
// is done.
void count_release(Decoder& d, u32 key)
{
    for (u32 i = 0; i < d.core->side_entries; i++) {
        auto& s = d.side[i];
        if (!s.used || s.key != key) continue;
        s.released++;
        if (s.expected != 0xFF && s.expected <= s.released && s.done) s.used = false;
        return;
    }
}

s32 release_frame(PPUContext& ctx, const Core& core, u32 handle, u32 frame_id)
{
    if (!handle) return core.arg();
    const std::shared_ptr<Decoder> d = decoder_of(ctx, core, handle);
    if (!d) return core.arg();
    std::lock_guard lk(d->m);
    if (d->destroyed) return lock_gone(*d);
    if (d->state != ST_FLUSHING && d->state != ST_FLUSHED && d->state != ST_DECODING) return core.seq();
    if (d->other_queue.size() + 2 > core.other_room) return core.busy();
    auto it = std::find_if(d->frames.begin(), d->frames.begin() + core.frame_entries,
        [&](const auto& e) { return e.used && e.key == frame_id; });
    if (it == d->frames.begin() + core.frame_entries) return core.arg();
    Command c;
    c.kind = Command::Kind::Release;
    c.count = it->count;
    c.side[0] = static_cast<u8>(it->side[0]);
    c.side[1] = static_cast<u8>(it->side[1]);
    c.frame_id = static_cast<u8>(frame_id);
    c.skipped = it->skipped;
    d->seq++;
    d->other_queue.push_back(c);
    for (u32 k = 0; k < it->count && k < 2; k++) {
        if (core.mvc) {
            count_release(*d, it->side[k]);
            continue;
        }
        for (u32 i = 0; i < core.side_entries; i++)
            if (d->side[i].used && d->side[i].key == it->side[k]) {
                d->side[i].used = false;
                break;
            }
    }
    it->used = false;
    wake(*d);
    return lv2::CELL_OK;
}

s32 flush_call(PPUContext& ctx, const Core& core, u32 handle, u32 mode)
{
    if (!handle || mode != 0) return core.arg();
    const std::shared_ptr<Decoder> d = decoder_of(ctx, core, handle);
    if (!d) return core.arg();
    std::lock_guard lk(d->m);
    if (d->destroyed) return lock_gone(*d);
    if (d->state != ST_DECODING && d->state != ST_READY) return core.seq();
    if (d->au_queue.size() >= core.au_room) return core.busy(); // the queue full: Flush checks no room first
    Command c;
    c.kind = Command::Kind::Flush;
    d->seq++;
    d->au_queue.push_back(c);
    d->state = ST_FLUSHING;
    wake(*d);
    return lv2::CELL_OK;
}

// ---- libavcdec

s32 avcDecQueryMemory(PPUContext& ctx, u32 param, u32 attr) { return query_memory(ctx, kAvc, param, attr); }

s32 avcDecOpen(PPUContext& ctx, u32 handle_out, u32 spurs, u32 taskset_out, u32 param, u32 memdesc, u32 stack, u32 sdk,
    u32 callback)
{
    u8* base = guest_base(ctx);
    const u32 sp = static_cast<u32>(ctx.gpr[1]);
    OpenArgs o;
    o.handle_out = handle_out;
    o.spurs = spurs;
    o.taskset_out = taskset_out;
    o.param = param;
    o.memdesc = memdesc;
    o.stack = stack;
    o.sdk = sdk;
    o.callback = callback;
    // The arguments past eight, in the caller's parameter save area.
    o.cb_arg = ps_read32(base, sp + 0x74);
    o.nspus = ps_read8(base, sp + 0x7F);
    o.prios = ps_read32(base, sp + 0x84);
    o.spu_prio = static_cast<s32>(ps_read32(base, sp + 0x8C));
    o.ppu_prio = static_cast<s32>(ps_read32(base, sp + 0x94));
    return open_decoder(ctx, kAvc, o);
}

s32 avcDecClose(PPUContext& ctx, u32 handle) { return close_decoder(ctx, kAvc, handle); }

s32 avcDecReset(PPUContext& ctx, u32 handle, u32 keep_sps, u32 flags) { return reset_call(ctx, kAvc, handle, keep_sps, flags); }

s32 avcDecAccessUnit(PPUContext& ctx, u32 handle, u32 addr, u32 size, u32 cont, u32 cont_size, u64 dts, u64 pts,
    u32 mode)
{
    u8* base = guest_base(ctx);
    const u32 sp = static_cast<u32>(ctx.gpr[1]);
    AuArgs a;
    a.addr = addr;
    a.size = size;
    a.cont = cont;
    a.cont_size = cont_size;
    a.dts = dts;
    a.pts = pts;
    a.mode = mode;
    a.side = ps_read32(base, sp + 0x74);
    a.side_size = ps_read32(base, sp + 0x7C);
    if (!handle || !addr || !size || (cont_size && !cont) || mode > 3 || a.side_size > 0x40 || (a.side_size && !a.side)) {
        PS_LOG_ERROR(kChan, "avcDecAccessUnit: bad parameter");
        return kAvc.arg();
    }
    return access_unit(ctx, kAvc, handle, a);
}

s32 avcDecRlsFrame(PPUContext& ctx, u32 handle, u32 frame_id) { return release_frame(ctx, kAvc, handle, frame_id); }

s32 avcDecFlush(PPUContext& ctx, u32 handle, u32 mode) { return flush_call(ctx, kAvc, handle, mode); }

// avcDecGetStatus(handle, status): [0] whether the access unit queue is full (Access Unit would be BUSY), [1] the
// access units given and not yet done.
s32 avcDecGetStatus(PPUContext& ctx, u32 handle, u32 status)
{
    if (!handle || !status || !writable(ctx, status, 8)) return kAvc.arg();
    const std::shared_ptr<Decoder> d = decoder_of(ctx, kAvc, handle);
    if (!d) return kAvc.arg();
    std::lock_guard lk(d->m);
    if (d->destroyed) return lock_gone(*d);
    if (d->state == ST_CLOSED) return kAvc.seq();
    u8* base = guest_base(ctx);
    ps_write32(base, status + 4, d->in_flight);
    ps_write32(base, status, d->au_queue.size() + 2 > kAvc.au_room ? 1 : 0);
    return lv2::CELL_OK;
}

// ---- libmvcdec

s32 mvcDecQueryMemory(PPUContext& ctx, u32 param, u32 attr) { return query_memory(ctx, kMvc, param, attr); }

// mvcDecOpen(&handle, spurs, &taskset, param, mem, stack, callback, arg, spus, priorities, spu priority, ppu priority):
// libavcdec's without the SDK version.
s32 mvcDecOpen(PPUContext& ctx, u32 handle_out, u32 spurs, u32 taskset_out, u32 param, u32 memdesc, u32 stack,
    u32 callback, u32 cb_arg)
{
    u8* base = guest_base(ctx);
    const u32 sp = static_cast<u32>(ctx.gpr[1]);
    OpenArgs o;
    o.handle_out = handle_out;
    o.spurs = spurs;
    o.taskset_out = taskset_out;
    o.param = param;
    o.memdesc = memdesc;
    o.stack = stack;
    o.callback = callback;
    o.cb_arg = cb_arg;
    o.nspus = ps_read8(base, sp + 0x77);
    o.prios = ps_read32(base, sp + 0x7C);
    o.spu_prio = static_cast<s32>(ps_read32(base, sp + 0x84));
    o.ppu_prio = static_cast<s32>(ps_read32(base, sp + 0x8C));
    return open_decoder(ctx, kMvc, o);
}

s32 mvcDecClose(PPUContext& ctx, u32 handle) { return close_decoder(ctx, kMvc, handle); }

s32 mvcDecReset(PPUContext& ctx, u32 handle, u32 keep_sps, u32 flags) { return reset_call(ctx, kMvc, handle, keep_sps, flags); }

// mvcDecAccessUnit(handle, pieces {base, size, its continuation, size, second view, size, its continuation, size}, dts,
// pts, mode, side data, size).
s32 mvcDecAccessUnit(PPUContext& ctx, u32 handle, u32 pieces, u64 dts, u64 pts, u32 mode, u32 side, u32 side_size)
{
    u8* base = guest_base(ctx);
    AuArgs a;
    if (pieces && writable(ctx, pieces, 32)) {
        a.addr = ps_read32(base, pieces);
        a.size = ps_read32(base, pieces + 4);
        a.cont = ps_read32(base, pieces + 8);
        a.cont_size = ps_read32(base, pieces + 12);
        a.dep = ps_read32(base, pieces + 16);
        a.dep_size = ps_read32(base, pieces + 20);
        a.dep_cont = ps_read32(base, pieces + 24);
        a.dep_cont_size = ps_read32(base, pieces + 28);
    }
    a.dts = dts;
    a.pts = pts;
    a.mode = mode;
    a.side = side;
    a.side_size = side_size;
    if (!handle || !pieces || !a.addr || !a.size || (a.cont_size && !a.cont) || (a.dep_size && !a.dep) ||
        (a.dep_size && a.dep_cont_size && !a.dep_cont) || mode > 3 || side_size > 0x40 || (side_size && !side)) {
        PS_LOG_ERROR(kMvc.library, "mvcDecAccessUnit: bad parameter");
        return kMvc.arg();
    }
    if (!a.dep_size) a.dep_cont_size = 0;
    return access_unit(ctx, kMvc, handle, a);
}

s32 mvcDecRlsFrame(PPUContext& ctx, u32 handle, u32 frame_id) { return release_frame(ctx, kMvc, handle, frame_id); }

s32 mvcDecFlush(PPUContext& ctx, u32 handle, u32 mode) { return flush_call(ctx, kMvc, handle, mode); }

// The helper threads' entries (internal functions of pspocket's cores): the decoder's handle their argument.
void helper_thread(PPUContext& ctx, u64 handle)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (const std::shared_ptr<Decoder> d = decoder_at(static_cast<u32>(handle))) run(ctx, *d);
    else PS_LOG_ERROR(kChan, "the helper thread started for an unknown decoder {:#x}", handle);
    lv2::_sys_ppu_thread_exit(t, 0);
}

void _pspocket_avcdec_thread(PPUContext& ctx, u64 handle) { helper_thread(ctx, handle); }
void _pspocket_mvcdec_thread(PPUContext& ctx, u64 handle) { helper_thread(ctx, handle); }

} // namespace

void register_avcdec(Registry& r)
{
    r.add("libavcdec", {
        PS_HLE_FUNC(0xDF6F03FC, avcDecQueryMemory),
        PS_HLE_FUNC(0x9D523894, avcDecOpen),
        PS_HLE_FUNC(0xC68B81BF, avcDecClose),
        PS_HLE_FUNC(0x3A92F3DF, avcDecReset),
        PS_HLE_FUNC(0xF1A9AF6F, avcDecAccessUnit),
        PS_HLE_FUNC(0xE1134B5D, avcDecRlsFrame),
        PS_HLE_FUNC(0x765DE1B4, avcDecFlush),
        PS_HLE_FUNC(0xD83259E8, avcDecGetStatus),
        PS_HLE_INTERNAL(0x2D9EA30D, _pspocket_avcdec_thread),
    });
}

void register_mvcdec(Registry& r)
{
    r.add("libmvcdec", {
        PS_HLE_FUNC(0x6BE252E1, mvcDecQueryMemory),
        PS_HLE_FUNC(0xC5C3B70A, mvcDecOpen),
        PS_HLE_FUNC(0x739B99DF, mvcDecClose),
        PS_HLE_FUNC(0x53F2DED8, mvcDecReset),
        PS_HLE_FUNC(0xAC46F509, mvcDecAccessUnit),
        PS_HLE_FUNC(0xEAB40886, mvcDecRlsFrame),
        PS_HLE_FUNC(0xCB6494D5, mvcDecFlush),
        PS_HLE_INTERNAL(0xC7AA1D3D, _pspocket_mvcdec_thread),
    });
}

} // namespace pspocket::hle::vdec::avc
