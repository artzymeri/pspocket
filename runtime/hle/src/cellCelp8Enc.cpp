// cellCelp8Enc: the MPEG-4 CELP encoder at 8 kHz (libcelp8enc), pspocket's own (D-932), made to answer and write what
// Sony's libcelp8enc does (compared call for call with libcelp8enc.sprx, tests/hle/test_celp8_compare.cpp). RPCS3's
// (rpcs3/Emu/Cell/Modules/cellCelp8Enc.cpp/.h) gave the structures and error codes.
//
// What libcelp8enc is: libcelpenc's design (cellCelpEnc.cpp) for the narrowband coder. A PPU library driving an SPU
// program (a SPURS task, on a SPURS instance of its own for Open, on the game's for OpenEx) through two SPURS queues; the
// encoder in the game's work memory, its handle the memory's first 128-byte boundary: the state (+0: closed 0, open 1,
// started 2, encoding 3, output ready 4), a frame's bytes (+0x28), the last answer's bytes (+0x2c), a copy of the frame
// (+0x1c80), the access unit or header the SPU program writes back (+0x2180) and the SPU program's own state (+0x2200);
// 0x7A80 bytes and at least 0x4000 after them. Start sends the configuration (the SPU program sets its encoder up and
// answers with the stream's header), EncodeFrame a frame (state 3), WaitForOutput takes the answer (state 4), GetAu
// copies the access unit (state 2); End and Close first take an answer still waiting. Here the SPU program's work is
// done on the host (celp_mpe_enc.cpp, its arithmetic bit for bit): a frame is encoded as EncodeFrame sends it. No SPURS
// instance, task set or queue is made; the states and the memory a caller can see are libcelp8enc's.
//
// What differs from libcelpenc: the codes (0x806140A1 on), the sizes, one OpenEx (no OpenExt), and the parameters:
// excitationMode 0 (multi-pulse), sampleRate 1 (8 kHz), configuration one of 0, 2, 6, 9, 12, 15, 18, 21, 24 and 26,
// wordSize 0 (floats; there is no 16-bit input).
//
// The structures: CellCelp8EncAttr {workMemSize, celpEncVerUpper, celpEncVerLower}; CellCelp8EncResource
// {totalMemSize, startAddr, ppuThreadPriority, spuThreadPriority, ppuThreadStackSize}; CellCelp8EncResourceEx
// {totalMemSize, startAddr, spurs, priority[8], maxContention}; CellCelp8EncParam {excitationMode, sampleRate,
// configuration, wordSize, outBuff, outSize}; CellCelp8EncPcmInfo and CellCelp8EncAuInfo {startAddr, size}.

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

constexpr std::string_view kChan = "cellCelp8Enc";

enum : u32 {
    CELL_CELP8ENC_ERROR_FAILED = 0x806140A1,
    CELL_CELP8ENC_ERROR_SEQ = 0x806140A2,
    CELL_CELP8ENC_ERROR_ARG = 0x806140A3,
    CELL_CELP8ENC_ERROR_CORE_FAILED = 0x806140B1,
    CELL_CELP8ENC_ERROR_CORE_SEQ = 0x806140B2,
    CELL_CELP8ENC_ERROR_CORE_ARG = 0x806140B3,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

enum : u32 { CLOSED = 0, OPEN = 1, STARTED = 2, ENCODING = 3, OUTPUT = 4 };

// The handle's memory (libcelp8enc's).
namespace off {
constexpr u32 state = 0, frame_bytes = 0x28, au_size = 0x2C, pcm = 0x1C80, out = 0x2180, spu_state = 0x2200;
} // namespace off
constexpr u32 kHandleSize = 0x7A80, kMinWork = 0x4000;
constexpr u32 kWorkMemSize = 0xBB00, kVersionUpper = 0x04000000, kVersionLower = 0x200;

struct Encoder {
    celp::MpeEncoder mpe;
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

// A pointer libcelp8enc would follow that is not guest memory: logged, ARG (libcelp8enc would fault).
s32 bad_pointer(std::string_view fn, std::string_view what, u32 addr)
{
    PS_LOG_ERROR(kChan, "{}: {} {:#x} is not guest memory", fn, what, addr);
    return error(CELL_CELP8ENC_ERROR_ARG);
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

// The handle's first 128-byte boundary in the work memory (libcelp8enc's).
u32 align_pad(u32 start) { return (128 - (start & 127)) & 127; }

// What libcelp8enc's open does once the handle's memory fits: the handle's state, frame and answer sizes zeroed; the
// memory after the handle (`remaining`) checked; the encoder made, state 1.
s32 open_at(PPUContext& ctx, u32 h, u32 remaining, u32 handle_out)
{
    u8* base = guest_base(ctx);
    if (remaining <= kMinWork - 1) return error(CELL_CELP8ENC_ERROR_ARG);
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

void clear_handle(u8* base, u32 h)
{
    ps_write32(base, h + off::state, 0);
    ps_write32(base, h + off::frame_bytes, 0);
    ps_write32(base, h + off::au_size, 0);
}

s32 cellCelp8EncQueryAttr(PPUContext& ctx, u32 attr)
{
    if (!attr) return error(CELL_CELP8ENC_ERROR_ARG);
    if (!in_guest(ctx, attr, 12)) return bad_pointer("cellCelp8EncQueryAttr", "attr", attr);
    u8* base = guest_base(ctx);
    ps_write32(base, attr, kWorkMemSize);
    ps_write32(base, attr + 4, kVersionUpper);
    ps_write32(base, attr + 8, kVersionLower);
    return lv2::CELL_OK;
}

// cellCelp8EncOpen(res, handle): a SPURS instance of its own (one SPU) at the priorities given. The resource is
// copied: the game's is left as it is.
s32 cellCelp8EncOpen(PPUContext& ctx, u32 res, u32 handle)
{
    if (!res || !handle) return error(CELL_CELP8ENC_ERROR_ARG);
    if (!in_guest(ctx, res, 16)) return bad_pointer("cellCelp8EncOpen", "res", res);
    u8* base = guest_base(ctx);
    const u32 total = ps_read32(base, res), start = ps_read32(base, res + 4);
    if (!start || ps_read32(base, res + 8) > 0xBFF || ps_read32(base, res + 12) > 0xFF)
        return error(CELL_CELP8ENC_ERROR_ARG);
    const u32 pad = align_pad(start);
    if (total < pad + kHandleSize) return error(CELL_CELP8ENC_ERROR_ARG);
    const u32 h = start + pad;
    if (!in_guest(ctx, h, kHandleSize)) return bad_pointer("cellCelp8EncOpen", "work memory", start);
    if (!in_guest(ctx, handle, 4)) return bad_pointer("cellCelp8EncOpen", "handle", handle);
    clear_handle(base, h);
    return open_at(ctx, h, total - pad - kHandleSize, handle);
}

// cellCelp8EncOpenEx(res, handle): a task set on the game's SPURS. The game's resource is changed (before the check of
// the memory left): its size less the handle's memory and alignment, its start past the handle's memory.
s32 cellCelp8EncOpenEx(PPUContext& ctx, u32 res, u32 handle)
{
    if (!res || !handle) return error(CELL_CELP8ENC_ERROR_ARG);
    if (!in_guest(ctx, res, 24)) return bad_pointer("cellCelp8EncOpenEx", "res", res);
    u8* base = guest_base(ctx);
    const u32 total = ps_read32(base, res), start = ps_read32(base, res + 4);
    const u32 contention = ps_read32(base, res + 20);
    if (!start || !ps_read32(base, res + 8) || !contention || contention > 8) return error(CELL_CELP8ENC_ERROR_ARG);
    const u32 pad = align_pad(start);
    if (total < pad + kHandleSize) return error(CELL_CELP8ENC_ERROR_ARG);
    const u32 h = start + pad;
    if (!in_guest(ctx, h, kHandleSize)) return bad_pointer("cellCelp8EncOpenEx", "work memory", start);
    if (!in_guest(ctx, handle, 4)) return bad_pointer("cellCelp8EncOpenEx", "handle", handle);
    clear_handle(base, h);
    ps_write32(base, res, total - pad - kHandleSize);
    ps_write32(base, res + 4, h + kHandleSize);
    return open_at(ctx, h, total - pad - kHandleSize, handle);
}

Encoder* encoder_of(u32 h)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    const auto it = s.encoders.find(h);
    return it == s.encoders.end() ? nullptr : it->second.get();
}

// The handle a call names: guest memory (libcelp8enc reads and writes it without a check).
bool valid_handle(PPUContext& ctx, u32 h, std::string_view fn)
{
    if (!in_guest(ctx, h, kHandleSize)) {
        PS_LOG_ERROR(kChan, "{}: handle {:#x} is not guest memory", fn, h);
        return false;
    }
    return true;
}

// libcelp8enc's End after its state check: an answer waiting is taken (state 4, then 2), the SPU program told to end,
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

s32 cellCelp8EncEnd(PPUContext& ctx, u32 h)
{
    if (!h) return error(CELL_CELP8ENC_ERROR_ARG);
    if (!valid_handle(ctx, h, "cellCelp8EncEnd")) return error(CELL_CELP8ENC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (read_state(base, h) <= OPEN) return error(CELL_CELP8ENC_ERROR_SEQ);
    return end_started(base, h);
}

s32 cellCelp8EncClose(PPUContext& ctx, u32 h)
{
    if (!h) return error(CELL_CELP8ENC_ERROR_ARG);
    if (!valid_handle(ctx, h, "cellCelp8EncClose")) return error(CELL_CELP8ENC_ERROR_ARG);
    u8* base = guest_base(ctx);
    const u32 st = read_state(base, h);
    if (st == CLOSED) return error(CELL_CELP8ENC_ERROR_SEQ);
    if (st >= STARTED) end_started(base, h);
    {
        State& s = state();
        std::lock_guard lock(s.mutex);
        s.encoders.erase(h);
    }
    set_state(base, h, CLOSED);
    return lv2::CELL_OK;
}

// cellCelp8EncStart(handle, param): from state 1 or 2 (2 ends first); the frame's bytes kept; the SPU program's
// encoder set up for the configuration and its header (29 bytes) copied to outBuff (outSize 0 without one).
s32 cellCelp8EncStart(PPUContext& ctx, u32 h, u32 param)
{
    if (!h) return error(CELL_CELP8ENC_ERROR_ARG);
    if (!param) return error(CELL_CELP8ENC_ERROR_ARG);
    if (!in_guest(ctx, param, 24)) return bad_pointer("cellCelp8EncStart", "param", param);
    if (!valid_handle(ctx, h, "cellCelp8EncStart")) return error(CELL_CELP8ENC_ERROR_ARG);
    u8* base = guest_base(ctx);
    const u32 mode = ps_read32(base, param), rate = ps_read32(base, param + 4);
    const u32 config = ps_read32(base, param + 8), word = ps_read32(base, param + 12);
    if (mode != 0 || rate != 1 || config > 26 || !celp::mpe_supported(config) || word != 0)
        return error(CELL_CELP8ENC_ERROR_ARG);
    const u32 st = read_state(base, h);
    if (st == CLOSED || st > STARTED) return error(CELL_CELP8ENC_ERROR_SEQ);
    ps_write32(base, h + off::frame_bytes, celp::kMpeConfigs[config].frame_size * 4);
    if (st == STARTED) end_started(base, h);
    Encoder* e = encoder_of(h);
    if (!e) {
        PS_LOG_ERROR(kChan, "cellCelp8EncStart: handle {:#x} has no encoder of libcelp8enc's behind it", h);
        return error(CELL_CELP8ENC_ERROR_CORE_FAILED);
    }
    u8 header[32];
    u32 size;
    {
        celp::spu::Scope scope;
        e->mpe.reset(config);
        size = celp::mpe_header(config, header);
    }
    std::memcpy(base + h + off::out, header, size);
    const u32 out = ps_read32(base, param + 16);
    if (out) {
        if (!in_guest(ctx, out, size)) return bad_pointer("cellCelp8EncStart", "outBuff", out);
        std::memcpy(base + out, base + h + off::out, size);
        ps_write32(base, param + 20, size);
    } else {
        ps_write32(base, param + 20, 0);
    }
    set_state(base, h, STARTED);
    return lv2::CELL_OK;
}

// cellCelp8EncEncodeFrame(handle, frameInfo): in state 2 only, a whole frame (its size the frame's bytes exactly); the
// frame copied and encoded (state 3).
s32 cellCelp8EncEncodeFrame(PPUContext& ctx, u32 h, u32 info)
{
    if (!h) return error(CELL_CELP8ENC_ERROR_ARG);
    if (!valid_handle(ctx, h, "cellCelp8EncEncodeFrame")) return error(CELL_CELP8ENC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (read_state(base, h) != STARTED) return error(CELL_CELP8ENC_ERROR_SEQ);
    if (!info) return error(CELL_CELP8ENC_ERROR_ARG);
    if (!in_guest(ctx, info, 8)) return bad_pointer("cellCelp8EncEncodeFrame", "frameInfo", info);
    const u32 start = ps_read32(base, info), size = ps_read32(base, info + 4);
    if (!start) return error(CELL_CELP8ENC_ERROR_ARG);
    const u32 bytes = ps_read32(base, h + off::frame_bytes);
    if (size != bytes) return error(CELL_CELP8ENC_ERROR_ARG);
    if (!in_guest(ctx, start, bytes)) return bad_pointer("cellCelp8EncEncodeFrame", "frame", start);
    std::memmove(base + h + off::pcm, base + start, bytes);
    Encoder* e = encoder_of(h);
    if (!e) {
        PS_LOG_ERROR(kChan, "cellCelp8EncEncodeFrame: handle {:#x} has no encoder of libcelp8enc's behind it", h);
        return error(CELL_CELP8ENC_ERROR_CORE_FAILED);
    }
    u8 au[64];
    u32 n;
    {
        celp::spu::Scope scope;
        n = e->mpe.encode_frame(base + h + off::pcm, au);
    }
    std::memcpy(base + h + off::out, au, n);
    e->pending = n; // the answer waits for WaitForOutput (or End or Close)
    set_state(base, h, ENCODING);
    return lv2::CELL_OK;
}

s32 cellCelp8EncWaitForOutput(PPUContext& ctx, u32 h)
{
    if (!h) return error(CELL_CELP8ENC_ERROR_ARG);
    if (!valid_handle(ctx, h, "cellCelp8EncWaitForOutput")) return error(CELL_CELP8ENC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (read_state(base, h) != ENCODING) return error(CELL_CELP8ENC_ERROR_SEQ);
    const Encoder* e = encoder_of(h);
    take_output(base, h, e ? e->pending : 0);
    return lv2::CELL_OK;
}

// cellCelp8EncGetAu(handle, outBuffer, auItem): in state 4 only; auItem's start is outBuffer, the access unit copied
// there (its size 0 without a buffer); state 2.
s32 cellCelp8EncGetAu(PPUContext& ctx, u32 h, u32 out, u32 item)
{
    if (!h) return error(CELL_CELP8ENC_ERROR_ARG);
    if (!item) return error(CELL_CELP8ENC_ERROR_ARG);
    if (!in_guest(ctx, item, 8)) return bad_pointer("cellCelp8EncGetAu", "auItem", item);
    if (!valid_handle(ctx, h, "cellCelp8EncGetAu")) return error(CELL_CELP8ENC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (read_state(base, h) != OUTPUT) return error(CELL_CELP8ENC_ERROR_SEQ);
    ps_write32(base, item, out);
    if (out) {
        const u32 size = ps_read32(base, h + off::au_size);
        if (!in_guest(ctx, out, size)) return bad_pointer("cellCelp8EncGetAu", "outBuffer", out);
        std::memmove(base + out, base + h + off::out, size);
        ps_write32(base, item + 4, size);
    } else {
        ps_write32(base, item + 4, 0);
    }
    set_state(base, h, STARTED);
    return lv2::CELL_OK;
}

} // namespace

void register_cellCelp8Enc(Registry& r)
{
    r.add("cellCelp8Enc", {
        PS_HLE_FUNC(0x2D677E0C, cellCelp8EncQueryAttr),
        PS_HLE_FUNC(0x2EB6EFEE, cellCelp8EncOpen),
        PS_HLE_FUNC(0xCD48AD62, cellCelp8EncOpenEx),
        PS_HLE_FUNC(0xFD2566B4, cellCelp8EncClose),
        PS_HLE_FUNC(0x0F6AB57B, cellCelp8EncStart),
        PS_HLE_FUNC(0xBBBC2C1C, cellCelp8EncEnd),
        PS_HLE_FUNC(0x2099F86E, cellCelp8EncEncodeFrame),
        PS_HLE_FUNC(0x29DA1EA6, cellCelp8EncWaitForOutput),
        PS_HLE_FUNC(0x48C5020D, cellCelp8EncGetAu),
    });
}

} // namespace pspocket::hle
