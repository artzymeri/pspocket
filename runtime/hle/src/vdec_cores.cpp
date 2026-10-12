// The video codec cores' own APIs, pspocket's own (D-920): the libraries Sony's libvdec drives (and a game may call
// directly) answering as Sony's cores do, over pspocket's decoding on FFmpeg (vdec_ffmpeg.hpp) and Sony's frame buffer
// layouts (vdec_layout.hpp). Each core's PPU side (its checks, states, picture queue and frame buffer bookkeeping) is
// reproduced from what Sony's module does; what its SPU program does with an access unit (decode it into the frame
// buffer the PPU side picked, and report the picture's type, the frame to show and the references kept) is done on the
// host. Compared call for call with each module in tests/hle/test_vdec_cores_compare.cpp.
//
// - libdivx311dec (DivX 3.11): divx311Dec*: QueryAttr, Open, Close, DecodeAu, GetNumOfPictures, GetPicture,
//   ReleasePicture, FlushPicture, Reset.
// - libdivxdec (DivX, MPEG-4 part 2 with B pictures and DivX's packed bitstream): divxDec*, the same nine.
// - libsmvd4 (MPEG-4 part 2, Simple profile): vdec_smvd4.cpp, over the s-cores' framework (vdec_score.hpp); registered
//   from here.
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
#include "vdec_core_common.hpp"
#include "vdec_ffmpeg.hpp"
#include "vdec_layout.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "vdecCores";
using vdec_core::call_import;
using vdec_core::error;
using vdec_core::writable;

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

// =================================================================================================== DivX

// libdivxdec's answers: its errors share libdivx311dec's codes (0x806155xx).
enum : u32 {
    DX_ERROR = 0x80615501,
    DX_ARG = 0x80615502,
    DX_SEQ = 0x80615503,
    DX_BUSY = 0x80615510,
    DX_AU = 0x80615520,
    DX_PIC = 0x80615540,
};
// Where libdivxdec keeps its open mark, and where its frame buffers begin past its memory's start: after its own state,
// the SPU tasks' context save areas and its overlay table (their sizes as Sony's module's SPU programs give them to
// cellSpurs and cellOvis, firmware 4.92), 128-byte aligned.
constexpr u32 kDxMagic = 0x52C, kDxState = 0x520;
constexpr u32 kDxContexts = 0x23C00 + 0x9C00, kDxOverlay = 0x174D0;
// Its states: opened, started, decoding, flushed, in error.
constexpr u32 DX_OPENED = 0x80, DX_STARTED = 0x81, DX_DECODING = 0x82, DX_FLUSHED = 0x83, DX_FAILED = 0x84;
// A picture entry: free 20, its access unit's words still to come (a packed bitstream's P picture) 21, decoded and
// waiting for its frame to be shown 22, ready 23.
constexpr u32 DXE_FREE = 20, DXE_LATE = 21, DXE_WAITING = 22, DXE_READY = 23;

// The picture types, frame sizes and largest sizes of libdivxdec's types 10 to 14 (QMOBILE to HD 1080).
constexpr u32 kDxTypeFrame[5] = {0xD680, 0x2D000, 0xA7700, 0x169500, 0x320A00};
constexpr u32 kDxTypeWidth[5] = {176, 352, 720, 1280, 1920}, kDxTypeHeight[5] = {144, 288, 576, 720, 1088};

// What GetPicture gives of a picture (0x40 bytes), filled when its access unit is decoded.
struct DxEntry {
    u32 state = DXE_FREE, frame = 0, size = 0;
    u32 user[4] = {};
    u8 aspect = 0, par_width = 0, par_height = 0, fixed = 0, skipped = 0;
    u16 resolution = 0, fixed_increment = 0, width = 0, height = 0, vop_time = 0, vop_seconds = 0;
    u32 type = 0, mark = 0x30, status = 0;
    u8 colour = 0, primaries = 0, transfer = 0, matrix = 0, top_first = 0, interlaced = 0;
    int next = 0, prev = 0;
};

// A frame buffer: not a reference 10, the backward reference 11, the forward one 12; the game's holds of it.
struct DxFrame {
    u32 state = 10, held = 0, extra = 0, addr = 0;
};

// MPEG-4 part 2 headers as the DivX core's SPU program reads them (ISO/IEC 14496-2 6.2.3, 6.2.5): the video object
// layer's fields it reports, and each video object plane's.
class HeaderBits {
public:
    HeaderBits(const u8* d, usize size) : d_(d), size_(size) {}
    u32 bits(u32 n)
    {
        u32 v = 0;
        for (u32 i = 0; i < n; i++, pos_++) v = (v << 1) | (pos_ / 8 < size_ ? (d_[pos_ / 8] >> (7 - pos_ % 8)) & 1 : 0);
        return v;
    }
    bool over() const { return pos_ > size_ * 8; }

private:
    const u8* d_;
    usize size_, pos_ = 0;
};

struct DxVol {
    bool seen = false;
    u32 width = 0, height = 0, verid = 1, shape = 0, sprite = 0;
    u16 resolution = 0, fixed_increment = 0;
    u8 aspect = 0, par_width = 0, par_height = 0;
    bool interlaced = false, newpred = false, reduced_resolution = false;
    u32 time_bits = 1;
};

void parse_dx_vol(const u8* p, usize n, DxVol& v)
{
    HeaderBits b(p, n);
    b.bits(1 + 8); // random_accessible_vol, video_object_type_indication
    v.verid = 1;
    if (b.bits(1)) { // is_object_layer_identifier
        v.verid = b.bits(4);
        b.bits(3);
    }
    v.aspect = static_cast<u8>(b.bits(4));
    v.par_width = v.par_height = 0;
    if (v.aspect == 15) {
        v.par_width = static_cast<u8>(b.bits(8));
        v.par_height = static_cast<u8>(b.bits(8));
    }
    if (b.bits(1)) { // vol_control_parameters: chroma_format, low_delay, vbv_parameters
        b.bits(3);
        if (b.bits(1)) b.bits(79);
    }
    v.shape = b.bits(2);
    if (v.shape == 3 && v.verid != 1) b.bits(4);
    b.bits(1);
    v.resolution = static_cast<u16>(b.bits(16));
    b.bits(1);
    v.time_bits = 1;
    while (v.time_bits < 16 && (1u << v.time_bits) < v.resolution) v.time_bits++;
    v.fixed_increment = b.bits(1) ? static_cast<u16>(b.bits(v.time_bits)) : 0;
    if (v.shape == 0) { // rectangular
        b.bits(1);
        v.width = b.bits(13);
        b.bits(1);
        v.height = b.bits(13);
        b.bits(1);
    }
    v.interlaced = b.bits(1) != 0;
    b.bits(1); // obmc_disable
    v.sprite = b.bits(v.verid == 1 ? 1 : 2);
    v.newpred = v.reduced_resolution = false;
    if (v.sprite == 0 && v.shape == 0) {
        if (b.bits(1)) b.bits(8); // not_8_bit: quant_precision, bits_per_pixel
        if (b.bits(1)) {          // quant_type: the matrices loaded, each to its first 0 (64 at most)
            for (int m = 0; m < 2; m++)
                if (b.bits(1))
                    for (int i = 0; i < 64 && b.bits(8) != 0; i++) {}
        }
        if (v.verid != 1) b.bits(1); // quarter_sample
        if (b.bits(1) == 0) {        // complexity estimation: its header is not read here
            v.seen = !b.over();
            return;
        }
        b.bits(1);                  // resync_marker_disable
        if (b.bits(1)) b.bits(1);   // data_partitioned, reversible_vlc
        if (v.verid != 1) {
            v.newpred = b.bits(1) != 0;
            if (v.newpred) b.bits(3);
            v.reduced_resolution = b.bits(1) != 0;
        }
    }
    v.seen = !b.over();
}

// A video object plane's header: its type (I 0, P 1, B 2, S 3), modulo time base (whole seconds), time increment,
// whether it is coded, and for an interlaced layer its top field first flag.
struct DxVop {
    u32 type = 0, seconds = 0, increment = 0;
    bool coded = true, top_first = false;
};

DxVop parse_dx_vop(const u8* p, usize n, const DxVol& v)
{
    DxVop o;
    HeaderBits b(p, n);
    o.type = b.bits(2);
    while (b.bits(1) && !b.over()) o.seconds++;
    b.bits(1);
    o.increment = b.bits(v.time_bits);
    b.bits(1);
    o.coded = b.bits(1) != 0;
    if (!o.coded || v.newpred) return o;
    if (v.shape != 2 && (o.type == 1 || (o.type == 3 && v.sprite == 2))) b.bits(1); // vop_rounding_type
    if (v.reduced_resolution && v.shape == 0 && o.type <= 1) b.bits(1);
    if (v.shape != 2) {
        b.bits(3); // intra_dc_vlc_thr
        if (v.interlaced) o.top_first = b.bits(1) != 0;
    }
    return o;
}

// Where the next start code (00 00 01 xx) begins at or after `from`, or `n`.
usize next_code(const u8* p, usize n, usize from)
{
    for (usize i = from; i + 3 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) return i;
    return n;
}

struct Divx {
    std::mutex m;
    u32 handle = 0, state = DX_OPENED, nframes = 0; // nframes: Open's buffers (libdivxdec's +0x530)
    u32 width = 0, height = 0;                      // what the SPU side reports (+0x38, +0x3C): Open's, then the VOL's
    std::vector<DxEntry> entries;
    int free_head = -1, out_head = -1, current = -1;
    u32 used = 0, ready = 0;
    std::vector<DxFrame> frames;
    u32 free_frames = 0, unheld = 0, extra_holds = 0;
    // The flags libdivxdec keeps: a packed bitstream's B picture taken (its N-VOP to come), the access unit's first VOP,
    // the last picture skipped, a picture decoded since Reset.
    bool packed_b = false, first_vop = false, last_skipped = false, decoded = false;
    u32 mark = 0x30; // the last picture's mark: 0x30, 0x31 (a packed P picture's words given), 0x32 (an N-VOP's)
    // What the SPU side reports of the last command, and keeps.
    DxVol vol;
    u32 type = 0, vop_time = 0, vop_seconds = 0, err = 0, show = 0, bwd = 0, fwd = 0;
    bool coded = true, more = false, top_first = false, have_ref = false;
    std::vector<u8> au; // the access unit being decoded (copied: the second VOP of a packed one comes later)
    usize au_pos = 0;
    vdec::FfDecoder ff;
    // Each frame buffer's picture as the SPU side keeps it: a reference (I, P) has its edges padded when the next VOP is
    // decoded (a B picture never, nor the last one before a flush); FFmpeg may hand an anchor picture over later.
    struct Pad {
        bool reference = false, written = false, due = false, padded = false;
        u32 width = 0, height = 0;
    };
    std::map<u32, Pad> pads;
};

struct Divxes {
    std::mutex m;
    std::map<u32, std::shared_ptr<Divx>> by_handle;
};

Divxes& divxes()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "libdivxdec called without an HLE process");
    return p->state<Divxes>();
}

std::shared_ptr<Divx> divx_of(PPUContext& ctx, u32 handle)
{
    if (!handle || !guest_readable(thread_of(ctx), handle + kDxMagic, 4)) return nullptr;
    if (ps_read32(guest_base(ctx), handle + kDxMagic) != kMagic) return nullptr;
    Divxes& all = divxes();
    std::lock_guard lk(all.m);
    const auto it = all.by_handle.find(handle);
    return it == all.by_handle.end() ? nullptr : it->second;
}

void dx_set_state(PPUContext& ctx, Divx& d, u32 state)
{
    d.state = state;
    ps_write32(guest_base(ctx), d.handle + kDxState, state);
}

// The status word GetPicture and DecodeAu give for the SPU side's error bits.
u32 dx_status(u32 err)
{
    u32 s = err & 0x80000000;
    if (err & 0x81000000) s = 0x81000000;
    if (err & 0x100000) s |= 0x82000000;
    if (err & 0x400) s |= 0x80000100;
    if (err & 1) s |= 0x44000000;
    if (err & 2) s |= 0x48000000;
    if (err & 0x100) s |= 0x100000;
    if (err & 0x40000) s |= 0x40800000;
    if (err & 0x200000) s |= 1;
    if (err & 4) s |= 2;
    if (err & 0x10) s |= 4;
    if (err & 0x20) s |= 8;
    if (err & 0x40) s |= 0x10;
    if (err & 0x200) s |= 0x20;
    if (err & 8) s |= 0x40020000;
    return s;
}

DxEntry& at(Divx& d, int i) { return d.entries[static_cast<usize>(i)]; }

// The entries' ring as new: each free, linked to its neighbours; the free and output heads at the first.
void dx_ring(Divx& d)
{
    const int n = static_cast<int>(d.entries.size());
    for (int i = 0; i < n; i++) {
        DxEntry& e = at(d, i);
        e.state = DXE_FREE;
        e.next = (i + 1) % n;
        e.prev = (i + n - 1) % n;
    }
    d.free_head = d.out_head = n ? 0 : -1;
    d.current = -1;
    d.used = d.ready = 0;
}

void dx_frames_reset(Divx& d)
{
    for (DxFrame& f : d.frames) f.state = 10, f.held = 0, f.extra = 0;
    d.free_frames = d.unheld = static_cast<u32>(d.frames.size());
    d.extra_holds = 0;
}

// Entry `c` linked into the ring before the free head (else the output head); nothing when there is neither.
void dx_insert(Divx& d, int c)
{
    const int before = d.free_head >= 0 ? d.free_head : d.out_head;
    if (before < 0) return;
    DxEntry& x = at(d, c);
    DxEntry& b = at(d, before);
    x.prev = b.prev;
    const int old_prev = b.prev;
    b.prev = c;
    x.next = at(d, old_prev).next;
    at(d, old_prev).next = c;
}

// A picture made ready (its frame shown) goes into the ring before the free entries; it is the output head when there is
// none or that is free.
void dx_link(Divx& d, int c)
{
    dx_insert(d, c);
    if (d.out_head < 0 || at(d, d.out_head).state == DXE_FREE) d.out_head = c;
}

// ---- the SPU side (D-920): its commands' effects on what it reports, the decoding on FFmpeg.

// The frame buffer layout of libdivxdec's pictures: DivX's strips (16 luma and 8 chroma lines of edge). A picture's
// samples are written as FFmpeg hands it over; a reference's edges once its padding is due (Sony's SPU program pads the
// forward reference when its next command comes, a decode or the flush; a B picture's edges stay as they were).
void dx_write_pictures(u8* base, Divx& d, std::deque<vdec::DecodedPicture>& out)
{
    const vdec::Plan plan;
    for (const vdec::DecodedPicture& p : out) {
        const u32 frame = static_cast<u32>(p.au);
        if (!frame) continue;
        vdec::write_strips(base, frame, plan, p.width, p.height, p.y.data(), p.u.data(), p.v.data(), false);
        if (p.damaged) d.err |= 0x200040; // reported as Sony's cores report a damaged picture (D-912)
        Divx::Pad& pad = d.pads[frame];
        pad.written = true;
        pad.width = p.width;
        pad.height = p.height;
        if (pad.reference && pad.due && !pad.padded) {
            vdec::pad_strips(base, frame, plan, p.width, p.height);
            pad.padded = true;
        }
    }
    out.clear();
}

// A command (a VOP's decoding, the flush) starts by padding the edges of the forward reference.
void dx_pad_reference(u8* base, Divx& d)
{
    if (!d.have_ref) return;
    Divx::Pad& pad = d.pads[d.fwd];
    if (!pad.reference || pad.padded) return;
    pad.due = true;
    if (!pad.written) return;
    vdec::pad_strips(base, d.fwd, vdec::Plan{}, pad.width, pad.height);
    pad.padded = true;
}

// Reset: the forward reference padded (FFmpeg's pictures written first), then the references and the stream forgotten.
void dx_spu_reset(u8* base, Divx& d)
{
    dx_pad_reference(base, d);
    std::deque<vdec::DecodedPicture> out;
    d.ff.drain(out);
    dx_write_pictures(base, d, out);
    d.have_ref = false;
    d.bwd = d.fwd = 0;
    d.pads.clear();
}

// Decode (the access unit's first VOP, or with `next` the one after the last): the VOP's headers read (a video object
// layer before it taken), then skipped by the decode mode, or decoded into frame `target`. The reply's bits: 0x100 done,
// 0x2000 a frame shown.
u32 dx_spu_decode(u8* base, Divx& d, u32 target, bool next, u32 command)
{
    dx_pad_reference(base, d);
    const u8* p = d.au.data();
    const usize n = d.au.size();
    usize i = next ? d.au_pos : 0;
    usize vop = n, headers = i;
    for (i = next_code(p, n, i); i + 4 <= n; i = next_code(p, n, i + 4)) {
        const u8 code = p[i + 3];
        if (code >= 0x20 && code <= 0x2F) {
            const usize end = next_code(p, n, i + 4);
            parse_dx_vol(p + i + 4, end - i - 4, d.vol);
            if (d.vol.seen && d.vol.shape == 0) {
                d.width = d.vol.width;
                d.height = d.vol.height;
            }
        } else if (code == 0xB6) {
            vop = i;
            break;
        }
    }
    d.err = 0;
    if (vop >= n || !d.vol.seen) {
        PS_LOG_WARN(kChan, "libdivxdec {:#x}: an access unit with no video object plane{} ({} bytes)", d.handle,
            d.vol.seen ? "" : " or layer", n);
        d.err = 0x200;
        d.show = 0;
        d.more = false;
        return 0x100;
    }
    const usize end = next_code(p, n, vop + 4);
    usize after = end;
    while (after < n && p[after + 3] != 0xB6) after = next_code(p, n, after + 4);
    d.more = after < n;
    d.au_pos = after;
    const DxVop o = parse_dx_vop(p + vop + 4, end - vop - 4, d.vol);
    d.type = o.type;
    d.vop_time = o.increment;
    d.vop_seconds = o.seconds;
    d.coded = o.coded;
    d.top_first = o.top_first;
    const bool skip = ((command & 0x4000) && o.type != 0) || ((command & 0x2000) && o.type == 2);
    if (!o.coded) { // an N-VOP: the last anchor picture shown again
        if (!d.have_ref) {
            d.err = 0x200;
            d.show = 0;
            return 0x100;
        }
        d.show = d.fwd;
        return 0x2100;
    }
    if (skip) { // not decoded: shown at once in the frame given, the references kept
        d.show = target;
        return 0x100 | (d.have_ref ? 0x2000u : 0u);
    }
    if (o.type != 0 && !d.have_ref) { // P, B or S with nothing to predict from
        d.err = 0x200;
        d.show = d.type = d.vop_time = d.vop_seconds = 0;
        d.bwd = d.fwd = 0;
        return 0x100;
    }
#if PSPOCKET_HAVE_FFMPEG
    // FFmpeg's floating point AAN IDCT: its pictures are the nearest to Sony's SPU program's (D-920).
    if (!d.ff.is_open() && !d.ff.open(AV_CODEC_ID_MPEG4, 0, 0, {}, FF_IDCT_FAAN))
        PS_LOG_ERROR(kChan, "libdivxdec {:#x}: FFmpeg's mpeg4 decoder did not open", d.handle);
#else
    PS_LOG_ERROR(kChan, "libdivxdec {:#x}: pspocket was built without FFmpeg: nothing is decoded", d.handle);
#endif
    // FFmpeg is given the VOP with the headers before it, one VOP a packet (a packed bitstream's apart).
    d.pads[target] = Divx::Pad{o.type != 2};
    std::deque<vdec::DecodedPicture> out;
    d.ff.decode(p + headers, static_cast<u32>(end - headers), target, out);
    dx_write_pictures(base, d, out);
    if (o.type == 2) { // B: shown at once, the references kept
        d.show = target;
        return 0x2100;
    }
    const bool had = d.have_ref;
    d.show = had ? d.fwd : target;
    d.bwd = had ? d.fwd : 0;
    d.fwd = target;
    d.have_ref = true;
    return 0x100 | (had ? 0x2000u : 0u);
}

// The flush: the last anchor picture (not shown yet) given, its edges padded, FFmpeg's pictures written, the references
// forgotten.
void dx_spu_flush(u8* base, Divx& d)
{
    dx_pad_reference(base, d);
    std::deque<vdec::DecodedPicture> out;
    d.ff.drain(out);
    dx_write_pictures(base, d, out);
    d.show = d.have_ref ? d.fwd : 0;
    d.have_ref = false;
}

// QueryAttr(param {type, width, height, frame buffers}, *memory size, *version): 2 to 4 frame buffers; types 10 to 14
// size for their level's largest frame, else the size given (16 to 1920 wide, 16 to 1088 high, or 1408x1152); the
// frames plus 0.5% plus 0x9E86B, to 16 bytes; the version 0x30806.
s32 divxDecQueryAttr(PPUContext& ctx, u32 param, u32 size_out, u32 version_out)
{
    u8* base = guest_base(ctx);
    if (!writable(ctx, param, 16) || !writable(ctx, size_out, 4) || !writable(ctx, version_out, 4))
        return error(DX_ARG);
    const u32 n = ps_read32(base, param + 12);
    if (n - 2 > 2) return error(DX_ARG);
    const u32 type = ps_read32(base, param);
    u32 frame = 0;
    if (type - 10 < 5) frame = kDxTypeFrame[type - 10];
    else {
        const u32 w = ps_read32(base, param + 4), h = ps_read32(base, param + 8);
        if (w - 0x10 < 0x771 && h < 0x441 && h > 0xF) frame = vdec::divx_frame_bytes(w, h);
        else if (w == 0x580 && h == 0x480) frame = 0x270600;
        else return error(DX_ARG);
    }
    const u32 frames = n * frame;
    const u32 size =
        frame ? (static_cast<u32>(static_cast<s64>(static_cast<double>(frames) * 0.005)) + frames + 0x9E86B) & ~0xFu
              : 0;
    ps_write32(base, size_out, size);
    ps_write32(base, version_out, 0x30806);
    return lv2::CELL_OK;
}

// Open(param, memory, size, taskset): the memory must hold libdivxdec's own 0x31B7C bytes, its SPU tasks' context save
// areas, its overlay table and the frame buffers; no memory, memory holding an open decoder, no parameters, a size
// refused as QueryAttr refuses it, no taskset or other than 2 to 4 frame buffers: ARG; the taskset's cellSpurs errors
// as they are. Each frame buffer is sized for the type's largest picture or the size given; the picture entries are as
// many. The handle is the memory.
s32 divxDecOpen(PPUContext& ctx, u32 param, u32 mem, u32 mem_size, u32 taskset)
{
    u8* base = guest_base(ctx);
    if (!mem || !writable(ctx, mem + kDxMagic, 4) || ps_read32(base, mem + kDxMagic) == kMagic || !writable(ctx, param, 16))
        return error(DX_ARG);
    const u32 type = ps_read32(base, param), n = ps_read32(base, param + 12);
    u32 w = 0, h = 0, frame = 0;
    if (type - 10 < 5) {
        frame = kDxTypeFrame[type - 10];
        w = kDxTypeWidth[type - 10];
        h = kDxTypeHeight[type - 10];
    } else {
        w = ps_read32(base, param + 4);
        h = ps_read32(base, param + 8);
        if (w - 0x10 < 0x771 && h < 0x441 && h > 0xF) frame = vdec::divx_frame_bytes(w, h);
        else if (w == 0x580 && h == 0x480) frame = 0x270600;
        else return error(DX_ARG);
    }
    if (mem_size < kDxOverlay + frame * n + kDxContexts + 0x31B7C) return error(DX_ARG);
    if (!taskset || n - 2 > 2) return error(DX_ARG);
    if (!writable(ctx, mem, vdec::align(mem + 0x580, 128) - mem + kDxContexts + kDxOverlay + 0x31400 + frame * n))
        return error(DX_ARG);
    auto d = std::make_shared<Divx>();
    d->handle = mem;
    d->nframes = n;
    d->width = w;
    d->height = h;
    dx_set_state(ctx, *d, DX_OPENED);
    if (const s32 r = check_taskset(ctx, taskset); r != lv2::CELL_OK) return r;
    // The frame buffers: past the state (+0x580), the two SPU tasks' context save areas and the overlay table, 128-byte
    // aligned, 0x20000 bytes on.
    const u32 tables = vdec::align(vdec::align(mem + 0x580, 128) + kDxContexts + 0x11400 + kDxOverlay, 128);
    d->frames.resize(n);
    for (u32 i = 0; i < n; i++) d->frames[i].addr = tables + 0x20000 + i * vdec::divx_frame_bytes(w, h);
    dx_frames_reset(*d);
    d->entries.resize(n);
    dx_ring(*d);
    ps_write32(base, mem + 0x38, w);
    ps_write32(base, mem + 0x3C, h);
    ps_write32(base, mem + 0x7C, n);
    dx_set_state(ctx, *d, DX_STARTED);
    {
        Divxes& all = divxes();
        std::lock_guard lk(all.m);
        all.by_handle[mem] = d;
    }
    ps_write32(base, mem + kDxMagic, kMagic);
    return lv2::CELL_OK;
}

// Close: the decoder's mark gone whatever the answer (a second Close is ARG); SEQ from a decoder opened but not started.
s32 divxDecClose(PPUContext& ctx, u32 handle)
{
    const std::shared_ptr<Divx> d = divx_of(ctx, handle);
    if (!d) return error(DX_ARG);
    s32 r = lv2::CELL_OK;
    {
        std::lock_guard lk(d->m);
        if (d->state == DX_FLUSHED || d->state == DX_STARTED || d->state == DX_DECODING || d->state == DX_FAILED) {
            d->ff.close();
            dx_set_state(ctx, *d, DX_OPENED);
        } else {
            r = error(DX_SEQ);
        }
    }
    ps_write32(guest_base(ctx), handle + kDxMagic, kClosed);
    Divxes& all = divxes();
    std::lock_guard lk(all.m);
    all.by_handle.erase(handle);
    return r;
}

// Reset(handle, mode): every frame buffer and picture entry free, the references forgotten (any mode: the SPU side's).
s32 divxDecReset(PPUContext& ctx, u32 handle, u32 mode)
{
    const std::shared_ptr<Divx> d = divx_of(ctx, handle);
    if (!d) return error(DX_ARG);
    std::lock_guard lk(d->m);
    if (d->state != DX_FLUSHED && d->state != DX_STARTED && d->state != DX_DECODING) return error(DX_SEQ);
    dx_frames_reset(*d);
    dx_ring(*d);
    d->first_vop = true;
    d->packed_b = d->decoded = d->last_skipped = false;
    ps_write32(guest_base(ctx), handle + 0x34, mode);
    dx_spu_reset(guest_base(ctx), *d);
    dx_set_state(ctx, *d, d->state == DX_STARTED ? DX_STARTED : DX_DECODING);
    return lv2::CELL_OK;
}

// One VOP of the access unit decoded (libdivxdec's decode step): a frame buffer picked (not a reference and not held;
// with two buffers the backward reference too), the SPU side run, the picture's entry made (shown at once, or waiting
// for its frame to be shown by the next picture), the frame buffers' references and holds set. `au_user`: the access
// unit's four words.
s32 dx_decode_step(PPUContext& ctx, Divx& d, const u32 au_user[4], u32 mode, u32 status, u32 command)
{
    u8* base = guest_base(ctx);
    if (mode == 0x21) command |= 0x4000;
    else if (mode == 0x22) command |= 0x2000;
    const u32 n = static_cast<u32>(d.frames.size());
    if (n <= 2 ? d.unheld == 0 : d.free_frames == 0) return error(DX_BUSY);
    int pick = -1;
    for (u32 i = 0; i < std::max<u32>(n, 1) && i < n; i++) {
        const DxFrame& f = d.frames[i];
        if ((n <= 2 ? (f.state == 10 || f.state == 11) : f.state == 10) && f.held == 0) {
            pick = static_cast<int>(i);
            break;
        }
    }
    if (pick < 0) return error(DX_BUSY);
    const u32 reply = dx_spu_decode(base, d, d.frames[static_cast<usize>(pick)].addr, command == 0x200, command);
    if (status) {
        ps_write32(base, status, 0);
        const u32 s = dx_status(d.err);
        ps_write32(base, status, s);
        if (s != 0 && static_cast<s32>(s) < 0) {
            dx_set_state(ctx, d, DX_FAILED);
            return error(DX_ERROR);
        }
    }
    if ((reply & 0xF00) != 0x100 || d.show == 0) return error(DX_AU);
    // (Sony's writes the status words below with no check of the pointer: a game always gives one.)
    if (d.type == 2 && d.nframes <= 2) {
        // B pictures need three frame buffers.
        if (status) ps_write32(base, status, 0x80000200);
        dx_set_state(ctx, d, DX_FAILED);
        return error(DX_ERROR);
    }
    u32& mark = d.mark;
    mark = 0x30;
    if (!d.coded) {
        if (d.packed_b) {
            // The N-VOP after a packed bitstream's B picture: its access unit's words are the waiting P picture's.
            d.packed_b = false;
            mark = 0x31;
            if (d.used > 0 && d.current >= 0 && at(d, d.current).state == DXE_LATE) {
                DxEntry& c = at(d, d.current);
                c.state = DXE_WAITING;
                std::copy(au_user, au_user + 4, c.user);
                return lv2::CELL_OK;
            }
            return error(DX_SEQ);
        }
        if (!d.last_skipped) {
            if (!d.decoded) {
                if (status) ps_write32(base, status, 0x40010000);
                return error(DX_AU);
            }
            mark = 0x32;
        }
    }
    const bool skip = mode == 0x21 ? d.type != 0 : mode == 0x22 ? d.type == 2 : false;
    d.last_skipped = skip;
    const bool shown = (reply & 0x2000) != 0;
    if (!shown && !skip && d.type != 0) {
        if (status) ps_write32(base, status, 0x40020000);
        return error(DX_AU);
    }
    // The picture's entry: the free head (or the one before it).
    bool added = false;
    int e = d.free_head;
    if (e < 0) return error(DX_BUSY);
    if (d.used >= d.entries.size()) {
        d.free_head = at(d, e).prev;
        return error(DX_BUSY);
    }
    if (at(d, e).state != DXE_FREE) {
        e = at(d, e).prev;
        d.free_head = e;
    }
    DxEntry& E = at(d, e);
    E.aspect = d.vol.aspect;
    E.mark = mark;
    E.par_width = d.vol.par_width;
    E.par_height = d.vol.par_height;
    E.resolution = d.vol.resolution;
    E.fixed = 1; // Sony's SPU program reports its word there set for every stream (whatever the layer's fixed_vop_rate)
    E.fixed_increment = d.vol.fixed_increment;
    E.width = static_cast<u16>(d.width);
    E.height = static_cast<u16>(d.height);
    E.type = d.type;
    E.status = dx_status(d.err);
    E.vop_time = static_cast<u16>(d.vop_time);
    E.vop_seconds = static_cast<u16>(d.vop_seconds);
    E.skipped = skip ? 1 : 0;
    // Sony's SPU program does not report the visual object's colour description: 0s.
    E.colour = E.primaries = E.transfer = E.matrix = 0;
    E.top_first = d.vol.interlaced && d.top_first ? 1 : 0;
    E.interlaced = d.vol.interlaced ? 1 : 0;
    E.size = vdec::divx_frame_bytes(d.width, d.height);
    if (!skip && E.type != 2) {
        // Out of the ring, alone: it waits for its frame to be shown (by the next picture, or the flush).
        const int next = E.next;
        d.free_head = e == next ? -1 : next;
        if (d.out_head == e) d.out_head = e == next ? -1 : next;
        at(d, E.prev).next = E.next;
        at(d, E.next).prev = E.prev;
        E.next = E.prev = e;
        if (d.more && d.first_vop && E.type == 1) {
            E.state = DXE_LATE; // a packed bitstream's P picture: its words come with the N-VOP
        } else {
            E.state = DXE_WAITING;
            std::copy(au_user, au_user + 4, E.user);
        }
        if (d.current >= 0) {
            DxEntry& C = at(d, d.current);
            if (C.state == DXE_WAITING) {
                C.state = DXE_READY;
                C.frame = d.show;
                dx_link(d, d.current);
                added = true;
            } else if (C.state == DXE_LATE) {
                // A packed P picture whose N-VOP never came: dropped, nothing shown; its entry the free head again.
                d.show = 0;
                d.packed_b = false;
                C.state = DXE_FREE;
                d.used--;
                dx_insert(d, d.current);
                d.free_head = d.current;
                if (d.out_head >= 0 && at(d, d.out_head).state == DXE_FREE) d.out_head = d.current;
            }
        }
        d.current = e;
    } else {
        // Shown at once (a B picture, or one skipped): ready now, its frame the SPU side's.
        std::copy(au_user, au_user + 4, E.user);
        E.frame = d.show;
        E.state = DXE_READY;
        d.free_head = e == E.next ? -1 : E.next;
        if (d.out_head < 0) d.out_head = e;
        added = true;
    }
    d.used++;
    // The frame buffers: the references (backward 11, forward 12), the others 10; the frame shown is the game's.
    const u32 shown_frame = shown || d.last_skipped ? d.show : 0;
    d.free_frames = 0;
    for (DxFrame& f : d.frames) {
        f.state = f.addr == d.bwd ? 11 : f.addr == d.fwd ? 12 : 10;
        if (f.addr == shown_frame) {
            if (f.held) {
                f.extra++;
                d.extra_holds++;
            } else {
                d.unheld--;
                f.held = 1;
            }
        } else if (f.held > 0) {
            f.held++;
        } else if (f.state == 10) {
            d.free_frames++;
        }
    }
    if (added) d.ready++;
    d.decoded = true;
    return lv2::CELL_OK;
}

// DecodeAu(handle, au {address, size, four words of the caller's}, mode, *status): mode 0x20 decodes every picture,
// 0x21 only I pictures, 0x22 all but B pictures. BUSY while the game holds as many pictures as it may (with three or
// more buffers, one buffer is kept back) or every picture entry but one is in use. A packed bitstream's access unit
// (a P VOP, then a B VOP) is decoded VOP by VOP: the B picture shown at once with the access unit's words, the P picture
// waiting for the N-VOP that gives its words; another second VOP is left (status 0x400).
s32 divxDecDecodeAu(PPUContext& ctx, u32 handle, u32 au, u32 mode, u32 status)
{
    const std::shared_ptr<Divx> d = divx_of(ctx, handle);
    if (!d) return error(DX_ARG);
    u8* base = guest_base(ctx);
    if (!writable(ctx, au, 0x18) || !ps_read32(base, au) || !ps_read32(base, au + 4) || mode - 0x20 > 2)
        return error(DX_ARG);
    if (status && !writable(ctx, status, 4)) return error(DX_ARG);
    std::lock_guard lk(d->m);
    Divx& x = *d;
    if (x.state != DX_DECODING && x.state != DX_STARTED) return error(DX_SEQ);
    const u32 n = static_cast<u32>(x.frames.size());
    bool room = false;
    if (n < 3) {
        for (const DxFrame& f : x.frames)
            if ((f.state == 10 || f.state == 11) && f.held == 0) {
                room = x.extra_holds < x.unheld;
                break;
            }
    } else {
        room = !(x.free_frames < 2 && x.unheld < 3) && x.extra_holds < x.unheld;
    }
    if (!room) return error(DX_BUSY);
    const u32 entries = static_cast<u32>(x.entries.size()) > 2 ? static_cast<u32>(x.entries.size()) - 1
                                                                 : static_cast<u32>(x.entries.size());
    if (x.used >= entries) return error(DX_BUSY);
    const u32 addr = ps_read32(base, au), size = ps_read32(base, au + 4);
    if (!writable(ctx, addr, size)) return error(DX_ARG);
    u32 user[4];
    for (u32 i = 0; i < 4; i++) user[i] = ps_read32(base, au + 8 + 4 * i);
    x.au.assign(base + addr, base + addr + size);
    x.au_pos = 0;
    if (status) ps_write32(base, status, 0);
    x.first_vop = true;
    s32 r = dx_decode_step(ctx, x, user, mode, status, 0x1200);
    if (r == lv2::CELL_OK) dx_set_state(ctx, x, DX_DECODING);
    else if (r != error(DX_AU)) return r;
    if (!x.more) return r;
    // Another VOP in the access unit (DivX's packed bitstream).
    x.first_vop = false;
    if (x.type == 1) {
        if (x.nframes < 3) {
            if (status) ps_write32(base, status, 0x80000200);
            dx_set_state(ctx, x, DX_FAILED);
            return error(DX_ERROR);
        }
        if (x.coded || x.mark != 0x31) {
            r = dx_decode_step(ctx, x, user, mode, status, 0x200);
            if (x.type == 2) x.packed_b = true;
            if (!x.more) return r;
        }
    }
    if (status) ps_write32(base, status, 0x400);
    return lv2::CELL_OK;
}

s32 divxDecGetNumOfPictures(PPUContext& ctx, u32 handle, u32 out)
{
    const std::shared_ptr<Divx> d = divx_of(ctx, handle);
    if (!d || !writable(ctx, out, 4)) return error(DX_ARG);
    std::lock_guard lk(d->m);
    if (d->state != DX_FLUSHED && d->state != DX_STARTED && d->state != DX_DECODING) return error(DX_ARG);
    ps_write32(guest_base(ctx), out, d->ready);
    return lv2::CELL_OK;
}

// GetPicture(handle, out): the output head's picture when it is ready (0x40 bytes: its frame, 0 for a skipped picture;
// the frame's size; the access unit's four words; the layer's aspect ratio, pixel sizes, time increment resolution, fixed
// rate and increment; the size; type, mark, status, skip byte; the VOP's time; colour and interlacing); PIC when its
// status has errors. Called when the head is not ready it still takes it off (as Sony's): SEQ.
s32 divxDecGetPicture(PPUContext& ctx, u32 handle, u32 out)
{
    const std::shared_ptr<Divx> d = divx_of(ctx, handle);
    if (!d) return error(DX_ARG);
    if (!writable(ctx, out, 0x40)) return error(DX_ARG);
    std::lock_guard lk(d->m);
    Divx& x = *d;
    if (x.state != DX_DECODING && x.state != DX_FLUSHED) return error(DX_SEQ);
    if (x.out_head < 0) return error(DX_SEQ);
    u8* base = guest_base(ctx);
    s32 r = error(DX_SEQ);
    const int h = x.out_head;
    DxEntry& E = at(x, h);
    if (x.ready > 0 && E.state == DXE_READY) {
        ps_write32(base, out + 0x00, E.skipped ? 0 : E.frame);
        ps_write32(base, out + 0x04, E.size);
        for (u32 i = 0; i < 4; i++) ps_write32(base, out + 0x08 + 4 * i, E.user[i]);
        ps_write8(base, out + 0x18, E.aspect);
        ps_write8(base, out + 0x19, E.par_width);
        ps_write8(base, out + 0x1A, E.par_height);
        ps_write16(base, out + 0x1C, E.resolution);
        ps_write8(base, out + 0x1E, E.fixed);
        ps_write16(base, out + 0x20, E.fixed_increment);
        ps_write16(base, out + 0x22, E.width);
        ps_write16(base, out + 0x24, E.height);
        ps_write32(base, out + 0x28, E.type);
        ps_write32(base, out + 0x2C, E.mark);
        ps_write32(base, out + 0x30, E.status);
        ps_write8(base, out + 0x34, E.skipped);
        ps_write16(base, out + 0x36, E.vop_time);
        ps_write16(base, out + 0x38, E.vop_seconds);
        ps_write8(base, out + 0x3A, E.colour);
        ps_write8(base, out + 0x3B, E.primaries);
        ps_write8(base, out + 0x3C, E.transfer);
        ps_write8(base, out + 0x3D, E.matrix);
        ps_write8(base, out + 0x3E, E.top_first);
        ps_write8(base, out + 0x3F, E.interlaced);
        r = E.status ? error(DX_PIC) : lv2::CELL_OK;
        x.ready--;
        x.used--;
    }
    E.state = DXE_FREE;
    if (h == E.next) {
        x.free_head = E.next;
        x.out_head = -1;
    } else {
        x.out_head = E.next;
    }
    return r;
}

// ReleasePicture: the frame the game has held longest is the core's again (or one hold of it less).
s32 divxDecReleasePicture(PPUContext& ctx, u32 handle)
{
    const std::shared_ptr<Divx> d = divx_of(ctx, handle);
    if (!d) return error(DX_ARG);
    std::lock_guard lk(d->m);
    Divx& x = *d;
    if (x.state != DX_DECODING && x.state != DX_FLUSHED) return error(DX_SEQ);
    DxFrame* oldest = nullptr;
    u32 most = 0;
    for (DxFrame& f : x.frames)
        if (f.held > most) {
            most = f.held;
            oldest = &f;
        }
    if (!oldest) return error(DX_SEQ);
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

// FlushPicture: the picture waiting for its frame becomes ready with the last anchor picture's (Sony's counts its entry
// once more, kept); a packed P picture still without its words is dropped; every frame buffer is no reference any more.
// From a started decoder: nothing.
s32 divxDecFlushPicture(PPUContext& ctx, u32 handle)
{
    const std::shared_ptr<Divx> d = divx_of(ctx, handle);
    if (!d) return error(DX_ARG);
    std::lock_guard lk(d->m);
    Divx& x = *d;
    if (x.state != DX_DECODING) return x.state == DX_STARTED ? lv2::CELL_OK : error(DX_SEQ);
    dx_spu_flush(guest_base(ctx), x);
    u32 shown = x.show;
    bool added = false;
    if (shown) {
        if (x.used < 1 || x.current < 0) {
            shown = 0;
        } else if (at(x, x.current).state == DXE_WAITING) {
            DxEntry& C = at(x, x.current);
            C.frame = shown;
            C.state = DXE_READY;
            dx_link(x, x.current);
            x.used++;
            x.current = -1;
            added = true;
        } else if (at(x, x.current).state == DXE_LATE) {
            shown = 0;
        } else {
            return error(DX_ARG);
        }
    }
    for (DxFrame& f : x.frames) {
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
    dx_set_state(ctx, x, DX_FLUSHED);
    return lv2::CELL_OK;
}

} // namespace

void register_vdec_cores(Registry& r)
{
    vdec_core::register_smvd4(r);
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
    r.add("libdivxdec", {
        PS_HLE_FUNC(0xA9022CC5, divxDecQueryAttr),
        PS_HLE_FUNC(0x7CFD1C1F, divxDecOpen),
        PS_HLE_FUNC(0xC6533FB2, divxDecClose),
        PS_HLE_FUNC(0x964AB1E7, divxDecDecodeAu),
        PS_HLE_FUNC(0x4AC10F4F, divxDecGetNumOfPictures),
        PS_HLE_FUNC(0x4D866CF0, divxDecGetPicture),
        PS_HLE_FUNC(0x2FA854AE, divxDecReleasePicture),
        PS_HLE_FUNC(0x586EBC8A, divxDecFlushPicture),
        PS_HLE_FUNC(0xF6016B2D, divxDecReset),
    });
}

} // namespace pspocket::hle
