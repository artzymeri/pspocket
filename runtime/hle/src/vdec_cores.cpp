// The video codec cores' own APIs, pspocket's own (D-920): the libraries Sony's libvdec drives (and a game may call
// directly) answering as Sony's cores do, over pspocket's decoding on FFmpeg (vdec_ffmpeg.hpp) and Sony's frame buffer
// layouts (vdec_layout.hpp). Each core's PPU side (its checks, states, picture queue and frame buffer bookkeeping) is
// reproduced from what Sony's module does; what its SPU program does with an access unit (decode it into the frame
// buffer the PPU side picked, and report the picture's type, the frame to show and the references kept) is done on the
// host. Compared call for call with each module in tests/hle/test_vdec_cores_compare.cpp.
//
// - libdivx311dec (DivX 3.11): divx311Dec*: QueryAttr, Open, Close, DecodeAu, GetNumOfPictures, GetPicture,
//   ReleasePicture, FlushPicture, Reset.
//
// Nothing here is Sony's code or data (CLAUDE.md rule 1): the behaviour is written from what the modules do.

#include <algorithm>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "vdec_ffmpeg.hpp"
#include "vdec_layout.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "vdecCores";
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

bool writable(PPUContext& ctx, u32 addr, u32 size) { return addr && guest_readable(thread_of(ctx), addr, size); }

// Another HLE library's function called as the core module calls its import (cellSpurs), on this thread.
u64 call_import(PPUContext& ctx, std::string_view module, u32 nid, std::initializer_list<u64> args)
{
    const Function* f = registry().find(module, nid);
    if (!f) PS_FATAL(kChan, "{}'s function {:#010x} is not pspocket's own", module, nid);
    u64 saved[8];
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 0;
    for (const u64 a : args) ctx.gpr[3 + i++] = a;
    f->fn(ctx, guest_base(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

// What Sony's cores check of the taskset they are given (their SPU tasks' home): its SPURS instance, and that
// instance's information; an error of cellSpurs's is the core's answer.
s32 check_taskset(PPUContext& ctx, u32 taskset)
{
    u8* base = guest_base(ctx);
    StackVar spurs(ctx, base, 16);
    StackVar info(ctx, base, 0x118);
    const s32 r = static_cast<s32>(call_import(ctx, "cellSpurs", 0x58D58FCF, {taskset, spurs.addr()})); // GetSpursAddress
    if (r != lv2::CELL_OK) return r;
    return static_cast<s32>(call_import(ctx, "cellSpurs", 0x1F402F8F, {ps_read32(base, spurs.addr()), info.addr()}));
}

// =================================================================================================== DivX 3.11

enum : u32 {
    D311_ERROR = 0x80615501, // the decoder failed (its SPU side): the decoder is in error
    D311_ARG = 0x80615502,
    D311_SEQ = 0x80615503,   // not in a state for the call
    D311_BUSY = 0x80615510,  // no frame buffer or picture entry free: release a picture and call again
    D311_AU = 0x80615520,    // the access unit gave no picture (a P picture with no reference, ...)
    D311_PIC = 0x80615540,   // GetPicture: the picture has errors (its status word says which)
};
constexpr u32 kMagic = 0xBABEFACE, kClosed = 0x88888888;
// Where libdivx311dec keeps its state word and its open mark in its memory, and where its frame buffers begin.
constexpr u32 kD311State = 0x3D640, kD311Magic = 0x3D64C, kD311Frames = 0x3D700;
// The decoder's states: opened (not started), started, decoding, flushed, in error.
constexpr u32 D311_OPENED = 0x80, D311_STARTED = 0x81, D311_DECODING = 0x82, D311_FLUSHED = 0x83, D311_FAILED = 0x84;

// A picture entry (libdivx311dec keeps two, in a ring): free 0x14, decoded and waiting for its frame to be shown 0x16,
// ready 0x17; what GetPicture gives of it.
struct D311Entry {
    u32 state = 0x14, frame = 0, size = 0, type = 0, mark = 0x30, status = 0;
    u32 user[4] = {};
    u16 width = 0, height = 0;
    u8 skipped = 0;
    int next = 0, prev = 0;
};

// A frame buffer: not a reference 10, the reference kept 11, the frame just decoded 12; the game's holds of it (the
// count grows with each picture shown after it: the oldest is released first).
struct D311Frame {
    u32 state = 10, held = 0, extra = 0, addr = 0;
};

struct Divx311 {
    std::mutex m;
    u32 handle = 0;
    u32 state = D311_OPENED;
    std::vector<D311Entry> entries;
    int free_head = 0, out_head = 0, current = -1;
    u32 used = 0, ready = 0;
    std::vector<D311Frame> frames;
    u32 free_frames = 0, unheld = 0, extra_holds = 0;
    // What the SPU side reports of the last access unit, and keeps: the picture's type and error word, the frame to
    // show, the references (the one before and the frame decoded), the last frame decoded and whether it is unshown.
    u32 type = 0, err = 0, show = 0, ref_before = 0, ref_now = 0;
    u32 last_frame = 0;
    bool have_ref = false, pending = false;
    vdec::FfDecoder ff;
    u32 ff_width = 0, ff_height = 0;
};

struct Divx311s {
    std::mutex m;
    std::map<u32, std::shared_ptr<Divx311>> by_handle;
};

Divx311s& divx311s()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "libdivx311dec called without an HLE process");
    return p->state<Divx311s>();
}

// The core at `handle` when its memory holds the open decoder's mark (as Sony's checks it), else none.
std::shared_ptr<Divx311> divx311_of(PPUContext& ctx, u32 handle)
{
    if (!handle || !guest_readable(thread_of(ctx), handle + kD311Magic, 4)) return nullptr;
    if (ps_read32(guest_base(ctx), handle + kD311Magic) != kMagic) return nullptr;
    Divx311s& all = divx311s();
    std::lock_guard lk(all.m);
    const auto it = all.by_handle.find(handle);
    return it == all.by_handle.end() ? nullptr : it->second;
}

void set_state(PPUContext& ctx, Divx311& d, u32 state)
{
    d.state = state;
    ps_write32(guest_base(ctx), d.handle + kD311State, state);
}

// The status word GetPicture gives for the SPU side's error word.
u32 d311_status(u32 err)
{
    u32 s = err & 0x80000000;
    if (err & 0x81000000) s = 0x81000000;
    if (err & 0x100000) s |= 0x82000000;
    if (err & 0x400) s |= 0x80000100;
    if (err & 2) s |= 0x48000000;
    if (err & 0x200000) s |= 1;
    if (err & 0x20) s |= 8;
    if (err & 0x40) s |= 0x10;
    if (err & 8) s |= 0x40020000;
    return s;
}

// The entries' ring as new: each free, linked to its neighbours.
void d311_ring(Divx311& d)
{
    const int n = static_cast<int>(d.entries.size());
    for (int i = 0; i < n; i++) {
        D311Entry& e = d.entries[static_cast<usize>(i)];
        e.state = 0x14;
        e.next = (i + 1) % n;
        e.prev = (i + n - 1) % n;
    }
    d.free_head = d.out_head = 0;
    d.current = -1;
    d.used = d.ready = 0;
}

D311Entry& at(Divx311& d, int i) { return d.entries[static_cast<usize>(i)]; }

// Entry `e` put into the ring before entry `before`.
void d311_insert(Divx311& d, int e, int before)
{
    D311Entry& x = at(d, e);
    D311Entry& b = at(d, before);
    const int prev = b.prev;
    x.prev = prev;
    b.prev = e;
    x.next = at(d, prev).next;
    at(d, prev).next = e;
}

// The ready entry `c` (its frame shown) linked into the ring before the free entries (or the output's head).
void d311_link_ready(Divx311& d, int c)
{
    int before = d.free_head;
    if (before < 0) before = d.out_head;
    if (before >= 0) d311_insert(d, c, before);
    if (before < 0 || d.out_head < 0 || at(d, d.out_head).state == 0x14) d.out_head = c;
}

// The SPU side's decode of an access unit into frame `frame` (D-920): the picture's type from its header (I 0, P 1);
// skipped by the decode mode (`skip_p`: all but I pictures; `skip_b`: B pictures, which DivX 3.11 has none of), it is
// not decoded and is shown at once in the frame given; a P picture with no reference is an error (8) and shows
// nothing; else it is decoded, the frame decoded before it is shown (the first: its own, though not shown yet) and it
// becomes the reference. The bits: 0x100 done, 0x2000 a frame was decoded before it.
u32 d311_spu_decode(u8* base, Divx311& d, u32 frame, u32 au, u32 size, u32 width, u32 height, bool skip_p, bool skip_b)
{
    d.type = ((ps_read8(base, au) >> 6) & 3) ? 1 : 0;
    d.err = 0;
    if ((skip_p && d.type != 0) || (skip_b && d.type == 2)) {
        d.show = frame;
        d.ref_before = 0;
        return 0x100;
    }
    if (!d.have_ref && d.type != 0) {
        d.err = 8;
        d.show = d.ref_before = d.ref_now = 0;
        return 0x100;
    }
#if PSPOCKET_HAVE_FFMPEG
    if (!d.ff.is_open() || d.ff_width != width || d.ff_height != height) {
        if (!d.ff.open(AV_CODEC_ID_MSMPEG4V3, width, height))
            PS_LOG_ERROR(kChan, "libdivx311dec {:#x}: FFmpeg's msmpeg4v3 decoder did not open", d.handle);
        d.ff_width = width;
        d.ff_height = height;
    }
#else
    PS_LOG_ERROR(kChan, "libdivx311dec {:#x}: pspocket was built without FFmpeg: nothing is decoded ({}x{})", d.handle,
        width, height);
#endif
    std::deque<vdec::DecodedPicture> out;
    d.ff.decode(base + au, size, 0, out);
    if (!out.empty()) {
        const vdec::DecodedPicture& p = out.back();
        const vdec::Plan plan; // DivX's strips: 16 luma and 8 chroma lines of edge
        vdec::write_strips(base, frame, plan, p.width, p.height, p.y.data(), p.u.data(), p.v.data());
        if (p.damaged) d.err = 0x200040; // the status word 0x11 (D-912)
    } else {
        PS_LOG_WARN(kChan, "libdivx311dec {:#x}: an access unit ({} bytes) gave FFmpeg no picture", d.handle, size);
        d.err = 0x200040;
    }
    const bool had = d.have_ref;
    d.show = had ? d.last_frame : frame;
    d.ref_before = had ? d.ref_now : 0;
    d.ref_now = frame;
    d.last_frame = frame;
    d.have_ref = d.pending = true;
    return 0x100 | (had ? 0x2000u : 0u);
}

// QueryAttr(param {type, width, height, frame buffers}, *memory size, *version): two frame buffers only; type 15 sizes
// for the largest frame (1920x1088), else the size given (16 to 1920 wide, 16 to 1088 high); the version 0x103.
s32 divx311DecQueryAttr(PPUContext& ctx, u32 param, u32 size_out, u32 version_out)
{
    u8* base = guest_base(ctx);
    if (!writable(ctx, param, 16) || !writable(ctx, size_out, 4) || !writable(ctx, version_out, 4))
        return error(D311_ARG);
    if (ps_read32(base, param + 12) != 2) return error(D311_ARG);
    u32 size = 0x68EC00;
    if (ps_read32(base, param) != 15) {
        const u32 w = ps_read32(base, param + 4), h = ps_read32(base, param + 8);
        if (w - 0x10 > 0x770 || h > 0x440 || h < 0x10) return error(D311_ARG);
        const u32 frame = vdec::divx_frame_bytes(w, h);
        size = frame ? frame * 2 + 0x4D800 : 0;
    }
    ps_write32(base, size_out, size);
    ps_write32(base, version_out, 0x103);
    return lv2::CELL_OK;
}

// Open(param, memory, size, taskset): the memory must hold the frame buffers past the decoder's own 0x3D77F bytes;
// memory already holding an open decoder, no parameters, no taskset or frame buffers other than two: ARG; the
// taskset's errors (cellSpurs's) as they are. The handle is the memory.
s32 divx311DecOpen(PPUContext& ctx, u32 param, u32 mem, u32 mem_size, u32 taskset)
{
    u8* base = guest_base(ctx);
    if (!mem || !writable(ctx, mem + kD311Magic, 4) || ps_read32(base, mem + kD311Magic) == kMagic ||
        !writable(ctx, param, 16))
        return error(D311_ARG);
    u32 w = 0x780, h = 0x440, frames_bytes = 0;
    const u32 n = ps_read32(base, param + 12);
    if (ps_read32(base, param) == 15) frames_bytes = n * 0x320A00;
    else {
        w = ps_read32(base, param + 4);
        h = ps_read32(base, param + 8);
        if (w - 0x10 > 0x770 || h > 0x440 || h < 0x10) return error(D311_ARG);
        frames_bytes = vdec::divx_frame_bytes(w, h) * n;
    }
    if (frames_bytes + 0x3D77F > mem_size) return error(D311_ARG);
    if (!taskset || n != 2) return error(D311_ARG);
    if (!writable(ctx, mem, frames_bytes + 0x3D77F)) return error(D311_ARG);
    auto d = std::make_shared<Divx311>();
    d->handle = mem;
    set_state(ctx, *d, D311_OPENED);
    if (const s32 r = check_taskset(ctx, taskset); r != lv2::CELL_OK) return r;
    // The frame buffers after the decoder's own memory, 128-byte aligned; the picture entries' ring.
    const u32 first = vdec::align(mem + kD311Frames, 128), bytes = vdec::divx_frame_bytes(w, h);
    d->frames.resize(n);
    for (u32 i = 0; i < n; i++) d->frames[i].addr = first + i * bytes;
    d->free_frames = d->unheld = n;
    d->entries.resize(2);
    d311_ring(*d);
    set_state(ctx, *d, D311_STARTED);
    {
        Divx311s& all = divx311s();
        std::lock_guard lk(all.m);
        all.by_handle[mem] = d;
    }
    ps_write32(base, mem + kD311Magic, kMagic);
    return lv2::CELL_OK;
}

// Close: the decoder's mark gone (a second Close is ARG); SEQ from a decoder opened but never started.
s32 divx311DecClose(PPUContext& ctx, u32 handle)
{
    const std::shared_ptr<Divx311> d = divx311_of(ctx, handle);
    if (!d) return error(D311_ARG);
    s32 r = lv2::CELL_OK;
    {
        std::lock_guard lk(d->m);
        if (d->state == D311_FLUSHED || d->state == D311_STARTED || d->state == D311_DECODING || d->state == D311_FAILED) {
            d->ff.close();
            set_state(ctx, *d, D311_OPENED);
        } else {
            r = error(D311_SEQ);
        }
    }
    ps_write32(guest_base(ctx), handle + kD311Magic, kClosed);
    Divx311s& all = divx311s();
    std::lock_guard lk(all.m);
    all.by_handle.erase(handle);
    return r;
}

// Reset(handle, mode): every frame buffer and picture entry free, the references forgotten. The mode is the SPU
// side's (pspocket's decoder does nothing different with it).
s32 divx311DecReset(PPUContext& ctx, u32 handle, u32 mode)
{
    const std::shared_ptr<Divx311> d = divx311_of(ctx, handle);
    if (!d) return error(D311_ARG);
    std::lock_guard lk(d->m);
    if (d->state != D311_FLUSHED && d->state != D311_STARTED && d->state != D311_DECODING) return error(D311_SEQ);
    for (D311Frame& f : d->frames) f.state = 10, f.held = 0, f.extra = 0;
    d->unheld = d->free_frames = static_cast<u32>(d->frames.size());
    d->extra_holds = 0;
    d311_ring(*d);
    if (mode) PS_LOG_DEBUG(kChan, "libdivx311dec {:#x}: Reset mode {:#x}", d->handle, mode);
    d->have_ref = d->pending = false;
    d->ref_now = d->ref_before = d->last_frame = 0;
    d->ff.reset();
    set_state(ctx, *d, d->state == D311_STARTED ? D311_STARTED : D311_DECODING);
    return lv2::CELL_OK;
}

// DecodeAu(handle, au {address, size, width, height, four words of the caller's}, mode, *status): mode 0x20 decodes
// every picture, 0x21 only I pictures, 0x22 all but B pictures; the access unit is decoded into the first frame
// buffer neither held by the game nor the reference to come. BUSY with no frame buffer or picture entry free. The
// picture before it (in decoding order) becomes ready with the frame shown.
s32 divx311DecDecodeAu(PPUContext& ctx, u32 handle, u32 au, u32 mode, u32 status)
{
    const std::shared_ptr<Divx311> d = divx311_of(ctx, handle);
    if (!d) return error(D311_ARG);
    u8* base = guest_base(ctx);
    if (!writable(ctx, au, 0x20) || !ps_read32(base, au) || !ps_read32(base, au + 4) || mode - 0x20 > 2)
        return error(D311_ARG);
    if (status && !writable(ctx, status, 4)) return error(D311_ARG);
    const u32 addr = ps_read32(base, au), size = ps_read32(base, au + 4);
    if (!writable(ctx, addr, size)) return error(D311_ARG);
    std::lock_guard lk(d->m);
    Divx311& x = *d;
    if (x.state != D311_DECODING && x.state != D311_STARTED) return error(D311_SEQ);
    // A frame buffer free (not held, not the reference to come) while the game holds fewer than it may; an entry free.
    bool free_frame = false;
    for (const D311Frame& f : x.frames)
        if ((f.state == 10 || f.state == 11) && f.held == 0) {
            free_frame = x.extra_holds < x.unheld;
            break;
        }
    if (!free_frame) return error(D311_BUSY);
    u32 entries = static_cast<u32>(x.entries.size());
    if (entries > 2) entries--;
    if (entries <= x.used) return error(D311_BUSY);
    const u32 aw = ps_read32(base, au + 8), ah = ps_read32(base, au + 12);
    u32 user[4];
    for (u32 i = 0; i < 4; i++) user[i] = ps_read32(base, au + 16 + 4 * i);
    if (status) ps_write32(base, status, 0);
    if (x.unheld == 0) return error(D311_BUSY);
    int pick = -1;
    for (usize i = 0; i < x.frames.size(); i++)
        if ((x.frames[i].state == 10 || x.frames[i].state == 11) && x.frames[i].held == 0) {
            pick = static_cast<int>(i);
            break;
        }
    if (pick < 0) return error(D311_BUSY);
    const u32 bits = d311_spu_decode(base, x, x.frames[static_cast<usize>(pick)].addr, addr, size, aw, ah, mode == 0x21,
        mode == 0x22);
    if (status) {
        const u32 s = d311_status(x.err);
        ps_write32(base, status, s);
        if (s != 0 && static_cast<s32>(s) < 0) {
            set_state(ctx, x, D311_FAILED);
            return error(D311_ERROR);
        }
    }
    if ((bits & 0xF00) != 0x100 || x.show == 0) return error(D311_AU);
    const bool skip = mode == 0x21 ? x.type != 0 : mode == 0x22 ? x.type == 2 : false;
    const bool shown_before = (bits & 0x2000) != 0;
    if (!shown_before && !skip && x.type != 0) {
        if (status) ps_write32(base, status, 0x40020000);
        return error(D311_AU);
    }
    // The picture's entry: the free head (or the one before it).
    bool added = false;
    int e = x.free_head;
    if (e < 0) return error(D311_BUSY);
    if (x.used >= x.entries.size()) {
        x.free_head = at(x, e).prev;
        return error(D311_BUSY);
    }
    if (at(x, e).state != 0x14) {
        e = at(x, e).prev;
        x.free_head = e;
    }
    D311Entry& E = at(x, e);
    E.width = static_cast<u16>(aw);
    E.height = static_cast<u16>(ah);
    E.type = x.type;
    E.mark = 0x30;
    E.status = d311_status(x.err);
    E.size = vdec::divx_frame_bytes(aw, ah);
    E.skipped = skip ? 1 : 0;
    const int next = E.next;
    x.free_head = e == next ? -1 : next;
    if (skip || E.type == 2) {
        // Shown at once (a skipped picture): ready now, its frame the SPU side's.
        std::copy(user, user + 4, E.user);
        E.frame = x.show;
        E.state = 0x17;
        if (x.out_head < 0) x.out_head = e;
        added = true;
    } else {
        // Out of the ring, alone: it waits for its frame to be shown (at the next access unit or the flush).
        if (x.out_head == e) x.out_head = e == E.next ? -1 : E.next;
        at(x, E.prev).next = E.next;
        at(x, E.next).prev = E.prev;
        E.next = E.prev = e;
        // (The SPU side always reports a picture whole, so libdivx311dec's entry kept for the next is never used.)
        std::copy(user, user + 4, E.user);
        E.state = 0x16;
        if (x.current >= 0 && at(x, x.current).state == 0x16) {
            D311Entry& C = at(x, x.current);
            C.frame = x.show;
            C.state = 0x17;
            d311_link_ready(x, x.current);
            added = true;
        }
        x.current = e;
    }
    x.used++;
    // The frame buffers: the reference kept 11, the frame decoded 12, the others 10; the frame shown is the game's.
    const u32 shown = shown_before || skip ? x.show : 0;
    x.free_frames = 0;
    for (D311Frame& f : x.frames) {
        f.state = f.addr == x.ref_before ? 11 : f.addr == x.ref_now ? 12 : 10;
        if (f.addr == shown) {
            if (f.held) {
                f.extra++;
                x.extra_holds++;
            } else {
                x.unheld--;
                f.held = 1;
            }
        } else if (f.held > 0) {
            f.held++;
        } else if (f.state == 10) {
            x.free_frames++;
        }
    }
    if (added) x.ready++;
    set_state(ctx, x, D311_DECODING);
    return lv2::CELL_OK;
}

s32 divx311DecGetNumOfPictures(PPUContext& ctx, u32 handle, u32 out)
{
    const std::shared_ptr<Divx311> d = divx311_of(ctx, handle);
    if (!d || !writable(ctx, out, 4)) return error(D311_ARG);
    std::lock_guard lk(d->m);
    if (d->state != D311_FLUSHED && d->state != D311_STARTED && d->state != D311_DECODING) return error(D311_ARG);
    ps_write32(guest_base(ctx), out, d->ready);
    return lv2::CELL_OK;
}

// GetPicture(handle, out): the oldest picture ready: its frame (0 for a skipped picture), its frame's size, the four
// words its access unit carried, its width and height (the access unit's), type, mark and status, and whether it was
// skipped; D311_PIC when its status says errors. Called with none ready it still takes the head entry off (as Sony's).
s32 divx311DecGetPicture(PPUContext& ctx, u32 handle, u32 out)
{
    const std::shared_ptr<Divx311> d = divx311_of(ctx, handle);
    if (!d) return error(D311_ARG);
    if (!writable(ctx, out, 0x2C)) return error(D311_ARG);
    std::lock_guard lk(d->m);
    Divx311& x = *d;
    if (x.state != D311_DECODING && x.state != D311_FLUSHED) return error(D311_SEQ);
    if (x.out_head < 0) return error(D311_SEQ);
    u8* base = guest_base(ctx);
    s32 r = error(D311_SEQ);
    const int h = x.out_head;
    D311Entry& E = at(x, h);
    if (x.ready > 0 && E.state == 0x17) {
        ps_write32(base, out + 0x00, E.skipped ? 0 : E.frame);
        ps_write32(base, out + 0x04, E.size);
        for (u32 i = 0; i < 4; i++) ps_write32(base, out + 0x08 + 4 * i, E.user[i]);
        ps_write16(base, out + 0x18, E.width);
        ps_write16(base, out + 0x1A, E.height);
        ps_write32(base, out + 0x1C, E.type);
        ps_write32(base, out + 0x20, E.mark);
        ps_write32(base, out + 0x24, E.status);
        ps_write8(base, out + 0x28, E.skipped);
        r = E.status ? error(D311_PIC) : lv2::CELL_OK;
        x.ready--;
        x.used--;
    }
    E.state = 0x14;
    if (h == E.next) {
        x.free_head = E.next;
        x.out_head = -1;
    } else {
        x.out_head = E.next;
    }
    return r;
}

// ReleasePicture: the frame the game has held longest is the core's again (or one hold of it less).
s32 divx311DecReleasePicture(PPUContext& ctx, u32 handle)
{
    const std::shared_ptr<Divx311> d = divx311_of(ctx, handle);
    if (!d) return error(D311_ARG);
    std::lock_guard lk(d->m);
    Divx311& x = *d;
    if (x.state != D311_DECODING && x.state != D311_FLUSHED) return error(D311_SEQ);
    D311Frame* oldest = nullptr;
    u32 most = 0;
    for (D311Frame& f : x.frames)
        if (f.held > most) {
            most = f.held;
            oldest = &f;
        }
    if (!oldest) return error(D311_SEQ);
    if (oldest->extra == 0) {
        x.unheld++;
        oldest->held = 0;
        if (oldest->state == 10) x.free_frames++;
    } else {
        oldest->extra--;
        x.extra_holds--;
    }
    return lv2::CELL_OK;
}

// FlushPicture: the picture still waiting for its frame to be shown becomes ready with the frame decoded last (and
// Sony's counts its entry once more); every frame buffer is no reference any more. From a started decoder: nothing.
s32 divx311DecFlushPicture(PPUContext& ctx, u32 handle)
{
    const std::shared_ptr<Divx311> d = divx311_of(ctx, handle);
    if (!d) return error(D311_ARG);
    std::lock_guard lk(d->m);
    Divx311& x = *d;
    if (x.state != D311_DECODING) return x.state == D311_STARTED ? lv2::CELL_OK : error(D311_SEQ);
    // The SPU side gives the frame it had not shown, and forgets it.
    u32 shown = x.pending ? x.last_frame : 0;
    x.pending = false;
    x.show = shown;
    bool added = false;
    if (shown) {
        if (x.used > 0 && x.current >= 0) {
            D311Entry& C = at(x, x.current);
            if (C.state != 0x16) return error(D311_ARG);
            C.frame = shown;
            C.state = 0x17;
            d311_link_ready(x, x.current);
            x.used++;
            x.current = -1;
            added = true;
        } else {
            shown = 0;
        }
    }
    for (D311Frame& f : x.frames) {
        if (f.addr == shown) {
            if (f.held) {
                f.extra++;
                x.extra_holds++;
            } else {
                f.held = 1;
            }
        } else if (f.held > 0) {
            f.held++;
        }
        f.state = 10;
    }
    if (added) x.ready++;
    set_state(ctx, x, D311_FLUSHED);
    return lv2::CELL_OK;
}

} // namespace

void register_vdec_cores(Registry& r)
{
    r.add("libdivx311dec", {
        PS_HLE_FUNC(0xA1521D39, divx311DecQueryAttr),
        PS_HLE_FUNC(0xBD6092C4, divx311DecOpen),
        PS_HLE_FUNC(0x5209A062, divx311DecClose),
        PS_HLE_FUNC(0x74444EF2, divx311DecFlushPicture),
        PS_HLE_FUNC(0x974DF255, divx311DecDecodeAu),
        PS_HLE_FUNC(0x3DCEA4DF, divx311DecGetPicture),
        PS_HLE_FUNC(0xD7764198, divx311DecGetNumOfPictures),
        PS_HLE_FUNC(0xB3C52B97, divx311DecReleasePicture),
        PS_HLE_FUNC(0xB4FD7E9B, divx311DecReset),
    });
}

} // namespace pspocket::hle
