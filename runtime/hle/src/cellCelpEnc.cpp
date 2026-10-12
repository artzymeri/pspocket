// cellCelpEnc: the MPEG-4 CELP encoder at 16 kHz (libcelpenc), pspocket's own (D-932), made to answer and write what
// Sony's libcelpenc does (compared call for call with libcelpenc.sprx, tests/hle/test_celp_compare.cpp). RPCS3's
// (rpcs3/Emu/Cell/Modules/cellCelpEnc.cpp/.h) gave the structures and error codes.
//
// What libcelpenc is: a PPU library driving an SPU program (a SPURS task, on a SPURS instance of its own for Open, on
// the game's for OpenEx and OpenExt) through two SPURS queues. The encoder lives in the game's work memory, its handle
// the memory's first 128-byte boundary: the state (+0: closed 0, open 1, started 2, encoding 3, output ready 4), a frame's
// bytes (+0x28), the last access unit's (+0x2c), a copy of the frame (+0x1c80), the access unit or header the SPU
// program writes back (+0x2080) and the SPU program's own state (+0x2100); 0x7D80 bytes and at least 0x2800 after them.
// Start sends the configuration (the SPU program sets its encoder up and answers with the stream's header), EncodeFrame
// a frame (state 3), WaitForOutput takes the answer (state 4), GetAu copies the access unit (state 2); End and Close
// first take an answer still waiting. Here the SPU program's work is done on the host (celp_rpe_enc.cpp, its arithmetic
// bit for bit): a frame is encoded as EncodeFrame sends it. No SPURS instance, task set or queue is made; the states and
// the memory a caller can see are libcelpenc's.
//
// The structures: CellCelpEncAttr {workMemSize, celpEncVerUpper, celpEncVerLower}; CellCelpEncResource {totalMemSize,
// startAddr, ppuThreadPriority, spuThreadPriority, ppuThreadStackSize}; CellCelpEncResourceEx {totalMemSize, startAddr,
// spurs, priority[8], maxContention}; CellCelpEncParam {excitationMode (RPE 1), sampleRate (16 kHz 2), configuration
// (0 to 3), wordSize (int16 little-endian 0, float 1), outBuff, outSize}; CellCelpEncPcmInfo and CellCelpEncAuInfo
// {startAddr, size}.

#include "celp_spu_fp.hpp" // first: the floating-point pragmas

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>

#include "celp_codec.hpp"
#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellCelpEnc";

enum : u32 {
    CELL_CELPENC_ERROR_FAILED = 0x80614001,
    CELL_CELPENC_ERROR_SEQ = 0x80614002,
    CELL_CELPENC_ERROR_ARG = 0x80614003,
    CELL_CELPENC_ERROR_CORE_FAILED = 0x80614081,
    CELL_CELPENC_ERROR_CORE_SEQ = 0x80614082,
    CELL_CELPENC_ERROR_CORE_ARG = 0x80614083,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

enum : u32 { CLOSED = 0, OPEN = 1, STARTED = 2, ENCODING = 3, OUTPUT = 4 };

// The handle's memory (libcelpenc's).
namespace off {
constexpr u32 state = 0, frame_bytes = 0x28, au_size = 0x2C, pcm = 0x1C80, out = 0x2080, spu_state = 0x2100;
} // namespace off
constexpr u32 kHandleSize = 0x7D80, kMinWork = 0x2800;
constexpr u32 kWorkMemSize = 0xA600, kVersionUpper = 0x04000000, kVersionLower = 0x100;

struct Encoder {
    celp::RpeEncoder rpe;
    bool float_input = false;
    u32 pending = 0; // the size of the access unit the SPU program has answered with, not taken yet
};

struct State {
    std::mutex mutex;
    std::map<u32, std::unique_ptr<Encoder>> encoders; // by handle
};

State& state() { return HleProcess::current()->state<State>(); }

bool in_guest(PPUContext& ctx, u32 addr, u32 size)
{
    if (!addr || u64{addr} + size > 0x100000000ull) return false;
    lv2::PpuThread& t = thread_of(ctx);
    for (u32 at = 0; at < size; at += 0x1000)
        if (!guest_readable(t, addr + at, std::min<u32>(size - at, 0x1000))) return false;
    return guest_readable(t, addr + size - 1, 1);
}

// A pointer libcelpenc would follow that is not guest memory: logged, ARG (libcelpenc would fault).
s32 bad_pointer(std::string_view fn, std::string_view what, u32 addr)
{
    PS_LOG_ERROR(kChan, "{}: {} {:#x} is not guest memory", fn, what, addr);
    return error(CELL_CELPENC_ERROR_ARG);
}

u32 read_state(u8* base, u32 h) { return ps_read32(base, h + off::state); }
void set_state(u8* base, u32 h, u32 s) { ps_write32(base, h + off::state, s); }

// The SPU program's answer to an encoded frame, taken (WaitForOutput, and End or Close in its place): the access
// unit's size kept, state 4.
void take_output(u8* base, u32 h, u32 size)
{
    ps_write32(base, h + off::au_size, size);
    set_state(base, h, OUTPUT);
}

// What libcelpenc's open does after its checks: the handle's state, frame and access unit sizes zeroed (before the
// remaining memory's check), the encoder made, state 1.
s32 open_at(PPUContext& ctx, u32 h, u32 remaining, u32 handle_out)
{
    u8* base = guest_base(ctx);
    ps_write32(base, h + off::state, 0);
    ps_write32(base, h + off::frame_bytes, 0);
    ps_write32(base, h + off::au_size, 0);
    if (remaining <= kMinWork - 1) return error(CELL_CELPENC_ERROR_ARG);
    // The first 128 bytes of the SPU program's state are zeroed as the task is made.
    std::memset(base + h + off::spu_state, 0, 0x80);
    {
        State& s = state();
        std::lock_guard lock(s.mutex);
        s.encoders[h] = std::make_unique<Encoder>();
    }
    set_state(base, h, OPEN);
    ps_write32(base, handle_out, h);
    return lv2::CELL_OK;
}

// The handle's first 128-byte boundary in the work memory and the memory it needs (libcelpenc's).
u32 align_pad(u32 start) { return (128 - (start & 127)) & 127; }

s32 cellCelpEncQueryAttr(PPUContext& ctx, u32 attr)
{
    if (!attr) return error(CELL_CELPENC_ERROR_ARG);
    if (!in_guest(ctx, attr, 12)) return bad_pointer("cellCelpEncQueryAttr", "attr", attr);
    u8* base = guest_base(ctx);
    ps_write32(base, attr, kWorkMemSize);
    ps_write32(base, attr + 4, kVersionUpper);
    ps_write32(base, attr + 8, kVersionLower);
    return lv2::CELL_OK;
}

// cellCelpEncOpen(res, handle): a SPURS instance of its own (one SPU) at the priorities given. The resource is copied:
// the game's is left as it is.
s32 cellCelpEncOpen(PPUContext& ctx, u32 res, u32 handle)
{
    if (!res || !handle) return error(CELL_CELPENC_ERROR_ARG);
    if (!in_guest(ctx, res, 16)) return bad_pointer("cellCelpEncOpen", "res", res);
    u8* base = guest_base(ctx);
    const u32 total = ps_read32(base, res), start = ps_read32(base, res + 4);
    if (!start || ps_read32(base, res + 8) > 0xBFF || ps_read32(base, res + 12) > 0xFF)
        return error(CELL_CELPENC_ERROR_ARG);
    const u32 pad = align_pad(start);
    if (total < pad + kHandleSize) return error(CELL_CELPENC_ERROR_ARG);
    const u32 h = start + pad;
    if (!in_guest(ctx, h, kHandleSize)) return bad_pointer("cellCelpEncOpen", "work memory", start);
    if (!in_guest(ctx, handle, 4)) return bad_pointer("cellCelpEncOpen", "handle", handle);
    return open_at(ctx, h, total - pad - kHandleSize, handle);
}

// cellCelpEncOpenEx and cellCelpEncOpenExt(res, handle) (one function in libcelpenc): a task set on the game's SPURS.
// The game's resource is changed: its size less the handle's memory and alignment, its start past the handle's memory.
s32 open_ex(PPUContext& ctx, u32 res, u32 handle, std::string_view fn)
{
    if (!res || !handle) return error(CELL_CELPENC_ERROR_ARG);
    if (!in_guest(ctx, res, 24)) return bad_pointer(fn, "res", res);
    u8* base = guest_base(ctx);
    const u32 total = ps_read32(base, res), start = ps_read32(base, res + 4);
    const u32 contention = ps_read32(base, res + 20);
    if (!start || !ps_read32(base, res + 8) || !contention || contention > 8) return error(CELL_CELPENC_ERROR_ARG);
    const u32 pad = align_pad(start);
    if (total < pad + kHandleSize) return error(CELL_CELPENC_ERROR_ARG);
    const u32 h = start + pad;
    if (!in_guest(ctx, h, kHandleSize)) return bad_pointer(fn, "work memory", start);
    if (!in_guest(ctx, handle, 4)) return bad_pointer(fn, "handle", handle);
    ps_write32(base, h + off::state, 0);
    ps_write32(base, h + off::frame_bytes, 0);
    ps_write32(base, h + off::au_size, 0);
    ps_write32(base, res, total - pad - kHandleSize);
    ps_write32(base, res + 4, h + kHandleSize);
    return open_at(ctx, h, total - pad - kHandleSize, handle);
}

s32 cellCelpEncOpenEx(PPUContext& ctx, u32 res, u32 handle) { return open_ex(ctx, res, handle, "cellCelpEncOpenEx"); }
s32 cellCelpEncOpenExt(PPUContext& ctx, u32 res, u32 handle) { return open_ex(ctx, res, handle, "cellCelpEncOpenExt"); }

Encoder* encoder_of(u32 h)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    const auto it = s.encoders.find(h);
    return it == s.encoders.end() ? nullptr : it->second.get();
}

// The handle a call names: guest memory, with an encoder of this library behind it when its state says there is one.
bool valid_handle(PPUContext& ctx, u32 h, std::string_view fn)
{
    if (!in_guest(ctx, h, kHandleSize)) {
        PS_LOG_ERROR(kChan, "{}: handle {:#x} is not guest memory", fn, h);
        return false;
    }
    return true;
}

// libcelpenc's End after its state check: an answer waiting is taken (state 4, then 2), the SPU program told to end,
// state 1.
s32 end_started(u8* base, u32 h)
{
    if (read_state(base, h) == ENCODING) {
        const Encoder* e = encoder_of(h);
        take_output(base, h, e ? e->pending : 0);
    }
    set_state(base, h, STARTED);
    set_state(base, h, OPEN);
    return lv2::CELL_OK;
}

s32 cellCelpEncEnd(PPUContext& ctx, u32 h)
{
    if (!h) return error(CELL_CELPENC_ERROR_ARG);
    if (!valid_handle(ctx, h, "cellCelpEncEnd")) return error(CELL_CELPENC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (read_state(base, h) <= OPEN) return error(CELL_CELPENC_ERROR_SEQ);
    return end_started(base, h);
}

s32 cellCelpEncClose(PPUContext& ctx, u32 h)
{
    if (!h) return error(CELL_CELPENC_ERROR_ARG);
    if (!valid_handle(ctx, h, "cellCelpEncClose")) return error(CELL_CELPENC_ERROR_ARG);
    u8* base = guest_base(ctx);
    const u32 st = read_state(base, h);
    if (st == CLOSED) return error(CELL_CELPENC_ERROR_SEQ);
    if (st >= STARTED) end_started(base, h);
    {
        State& s = state();
        std::lock_guard lock(s.mutex);
        s.encoders.erase(h);
    }
    set_state(base, h, CLOSED);
    return lv2::CELL_OK;
}

// cellCelpEncStart(handle, param): from state 1 or 2 (2 ends first); the frame's bytes kept; the SPU program's
// encoder set up for the configuration and its header (28 bytes) copied to outBuff (outSize 0 without one).
s32 cellCelpEncStart(PPUContext& ctx, u32 h, u32 param)
{
    if (!h) return error(CELL_CELPENC_ERROR_ARG);
    if (!param) return error(CELL_CELPENC_ERROR_ARG);
    if (!in_guest(ctx, param, 24)) return bad_pointer("cellCelpEncStart", "param", param);
    if (!valid_handle(ctx, h, "cellCelpEncStart")) return error(CELL_CELPENC_ERROR_ARG);
    u8* base = guest_base(ctx);
    const u32 mode = ps_read32(base, param), rate = ps_read32(base, param + 4);
    const u32 config = ps_read32(base, param + 8), word = ps_read32(base, param + 12);
    if (mode != 1 || rate != 2 || config > 3 || word > 1) return error(CELL_CELPENC_ERROR_ARG);
    const u32 st = read_state(base, h);
    if (st == CLOSED || st > STARTED) return error(CELL_CELPENC_ERROR_SEQ);
    // A frame: 160 samples in configuration 1, 240 in the others; floats or 16-bit samples.
    const u32 samples = config == 1 ? 160 : 240;
    ps_write32(base, h + off::frame_bytes, samples * (word ? 4 : 2));
    if (st == STARTED) end_started(base, h);
    Encoder* e = encoder_of(h);
    if (!e) {
        PS_LOG_ERROR(kChan, "cellCelpEncStart: handle {:#x} has no encoder of libcelpenc's behind it", h);
        return error(CELL_CELPENC_ERROR_CORE_FAILED);
    }
    e->float_input = word == 1;
    u8 header[32];
    u32 size;
    {
        celp::spu::Scope scope;
        e->rpe.reset(config);
        size = celp::rpe_header(config, header);
    }
    std::memcpy(base + h + off::out, header, size);
    const u32 out = ps_read32(base, param + 16);
    if (out) {
        if (!in_guest(ctx, out, size)) return bad_pointer("cellCelpEncStart", "outBuff", out);
        std::memcpy(base + out, base + h + off::out, size);
        ps_write32(base, param + 20, size);
    } else {
        ps_write32(base, param + 20, 0);
    }
    set_state(base, h, STARTED);
    return lv2::CELL_OK;
}

// cellCelpEncEncodeFrame(handle, frameInfo): in state 2 only, a whole frame (its size the frame's bytes exactly); the
// frame copied and encoded (state 3).
s32 cellCelpEncEncodeFrame(PPUContext& ctx, u32 h, u32 info)
{
    if (!h) return error(CELL_CELPENC_ERROR_ARG);
    if (!valid_handle(ctx, h, "cellCelpEncEncodeFrame")) return error(CELL_CELPENC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (read_state(base, h) != STARTED) return error(CELL_CELPENC_ERROR_SEQ);
    if (!info) return error(CELL_CELPENC_ERROR_ARG);
    if (!in_guest(ctx, info, 8)) return bad_pointer("cellCelpEncEncodeFrame", "frameInfo", info);
    const u32 start = ps_read32(base, info), size = ps_read32(base, info + 4);
    if (!start) return error(CELL_CELPENC_ERROR_ARG);
    const u32 bytes = ps_read32(base, h + off::frame_bytes);
    if (size != bytes) return error(CELL_CELPENC_ERROR_ARG);
    if (!in_guest(ctx, start, bytes)) return bad_pointer("cellCelpEncEncodeFrame", "frame", start);
    std::memmove(base + h + off::pcm, base + start, bytes);
    Encoder* e = encoder_of(h);
    if (!e) {
        PS_LOG_ERROR(kChan, "cellCelpEncEncodeFrame: handle {:#x} has no encoder of libcelpenc's behind it", h);
        return error(CELL_CELPENC_ERROR_CORE_FAILED);
    }
    u8 au[64];
    u32 n;
    {
        celp::spu::Scope scope;
        n = e->rpe.encode_frame(base + h + off::pcm, e->float_input, au);
    }
    std::memcpy(base + h + off::out, au, n);
    e->pending = n; // the answer waits for WaitForOutput (or End or Close)
    set_state(base, h, ENCODING);
    return lv2::CELL_OK;
}

s32 cellCelpEncWaitForOutput(PPUContext& ctx, u32 h)
{
    if (!h) return error(CELL_CELPENC_ERROR_ARG);
    if (!valid_handle(ctx, h, "cellCelpEncWaitForOutput")) return error(CELL_CELPENC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (read_state(base, h) != ENCODING) return error(CELL_CELPENC_ERROR_SEQ);
    const Encoder* e = encoder_of(h);
    take_output(base, h, e ? e->pending : 0);
    return lv2::CELL_OK;
}

// cellCelpEncGetAu(handle, outBuffer, auItem): in state 4 only; auItem's start is outBuffer, the access unit copied
// there (its size 0 without a buffer); state 2.
s32 cellCelpEncGetAu(PPUContext& ctx, u32 h, u32 out, u32 item)
{
    if (!h) return error(CELL_CELPENC_ERROR_ARG);
    if (!item) return error(CELL_CELPENC_ERROR_ARG);
    if (!in_guest(ctx, item, 8)) return bad_pointer("cellCelpEncGetAu", "auItem", item);
    if (!valid_handle(ctx, h, "cellCelpEncGetAu")) return error(CELL_CELPENC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (read_state(base, h) != OUTPUT) return error(CELL_CELPENC_ERROR_SEQ);
    ps_write32(base, item, out);
    if (out) {
        const u32 size = ps_read32(base, h + off::au_size);
        if (!in_guest(ctx, out, size)) return bad_pointer("cellCelpEncGetAu", "outBuffer", out);
        std::memmove(base + out, base + h + off::out, size);
        ps_write32(base, item + 4, size);
    } else {
        ps_write32(base, item + 4, 0);
    }
    set_state(base, h, STARTED);
    return lv2::CELL_OK;
}

} // namespace

void register_cellCelpEnc(Registry& r)
{
    r.add("cellCelpEnc", {
        PS_HLE_FUNC(0x6B148570, cellCelpEncQueryAttr),
        PS_HLE_FUNC(0x77B3B29A, cellCelpEncOpen),
        PS_HLE_FUNC(0x9EB084DB, cellCelpEncOpenEx),
        PS_HLE_FUNC(0xBA50BC23, cellCelpEncOpenExt),
        PS_HLE_FUNC(0x15EC0CCA, cellCelpEncClose),
        PS_HLE_FUNC(0x55DC23DE, cellCelpEncStart),
        PS_HLE_FUNC(0xF2B85DFF, cellCelpEncEnd),
        PS_HLE_FUNC(0x81FE030C, cellCelpEncEncodeFrame),
        PS_HLE_FUNC(0x9B244272, cellCelpEncWaitForOutput),
        PS_HLE_FUNC(0x3773692F, cellCelpEncGetAu),
    });
}

} // namespace pspocket::hle
