// sys_lv2coredump: liblv2coredump's crash dump writer, pspocket's own (D-943). Every step is what Sony's
// liblv2coredump.sprx (4.92) does, read from its code (the addresses below are its functions) and compared with it in
// tests/hle/test_lv2coredump_compare.cpp: the same lv2 calls in the same order, the same calls into the libraries it
// imports (liblv2's heap, lightweight mutex, thread and printf functions, cellGame, cellSysutil; all pspocket's own),
// the same blocks of its heap at the same places, the same bytes in the core file and the same answers.
//
// What liblv2coredump is: a module the system's libsysmodule loads as a process starts when the executable declares
// SYS_PROCESS_CRASH_DUMP_ENABLED (lv2's _sys_process_get_paramsfo flag 0x10, D-943). Its start checks the declaration,
// makes a 64 KB heap, an event queue it registers as the process's PPU exception handler
// (sys_dbg_initialize_ppu_exception_handler; the kernel knows it by its name "_l2cDBG"), a lightweight mutex for the
// user's log areas, and a thread, "__COREDUMP_HANDLER__", that waits on the queue; it also reads the game's title,
// title ID and version through libsysutil_game. Its export sys_lv2coredump_D725F320(fd) writes the process's state into
// the open file `fd` as a core file: an ELF64 core (OS ABI 0x66, PPC64) of PT_NOTE program headers, each one note named
// for what it holds, with a progress report on the TTY ("Generating Core Dump File", a dot each 4 %, " Done.").
//
// pspocket's console is a retail one: lv2 reports no debugging console's crash dump settings, so module start takes
// the declaration's configuration (a crash report to /dev_hdd0, no extra parts) and a core file holds what a retail
// console's does: SYSTEM_INFO, DUMP_CAUSE_INFO, PROCESS_INFO, GAME_APP_INFO (and the memory part, always empty there).
// The parts a debugging console's settings add (the PPU and SPU threads and registers, the synchronisation objects,
// memory, modules, SPURS) are not here. Not here yet either: the user's log areas (USER_LOG_INFO, filled by
// sys_crash_dump_set_user_log_area, which pspocket's sys_crashdump keeps apart) and the handler thread's crash report
// for an exception (pspocket's lv2 sends the queue no exception yet).
//
// The module's globals (its data segment) are host state here, except what lv2 or another library keeps an address
// of (the lightweight mutex's sys_lwmutex_t, the event queue's ID word): those are in guest memory of the module's own
// (State::data). Buffers the calls write into are blocks of the module's heap, as Sony's module allocates them, or its
// caller's stack.

#include "sys_lv2coredump.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sys_lv2coredump";
constexpr std::string_view kModule = "sys_lv2coredump";

constexpr u32 CELL_OK = 0;
constexpr u32 CELL_EAGAIN_ALREADY = 0x80010002; // liblv2coredump's "already started"
constexpr u32 CELL_ENOMEM = 0x80010004;
constexpr u32 CELL_ESRCH = 0x80010005;
constexpr u32 CELL_ENOSPC = 0x80010023;
constexpr u32 CELL_EIO = 0x8001002B;
constexpr u32 CELL_ENOTSUP = 0x80010037;

// ------------------------------------------------------------------------------------------------- other libraries

const Function* find(std::string_view module, u32 nid)
{
    static std::mutex m;
    static std::unordered_map<std::string, const Function*> cache;
    std::lock_guard lock(m);
    std::string key = std::format("{}:{:08x}", module, nid);
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    const Function* f = registry().find(module, nid);
    cache.emplace(std::move(key), f);
    return f;
}

// An import of liblv2coredump's (sysPrxForUser, cellGame, cellSysutil, cellSpurs), called as its stub would: the
// arguments in r3.., r3 back. One the host lacks answers what the loader's stub for it answers (CELL_ENOSYS).
u64 call_nid(PPUContext& ctx, std::string_view module, u32 nid, std::initializer_list<u64> args)
{
    const Function* f = find(module, nid);
    if (!f) {
        PS_LOG_ERROR(kChan, "UNIMPLEMENTED {}::{:#010x} (imported by liblv2coredump): CELL_ENOSYS", module, nid);
        return lv2::to_r3(lv2::err(lv2::CELL_ENOSYS));
    }
    std::array<u64, 8> saved{};
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 0;
    for (const u64 a : args) ctx.gpr[3 + i++] = a;
    f->fn(ctx, guest_base(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

u64 lv2(PPUContext& ctx, std::string_view name, std::initializer_list<u64> args)
{
    return call_nid(ctx, "sysPrxForUser", recomp::nid_of(name), args);
}

// An lv2 call as the module makes it (its own `sc`): r3.
u64 sc(PPUContext& ctx, u32 number, std::initializer_list<u64> args) { return lv2_syscall(ctx, number, args); }
s32 sc32(PPUContext& ctx, u32 number, std::initializer_list<u64> args)
{
    return static_cast<s32>(lv2_syscall(ctx, number, args));
}

// The TTY text liblv2coredump prints through _sys_printf (its strings have no conversions but one).
void print(PPUContext& ctx, std::string_view text)
{
    u8* b = guest_base(ctx);
    StackVar s(ctx, b, static_cast<u32>(text.size() + 1));
    std::memcpy(b + s.addr(), text.data(), text.size());
    lv2(ctx, "_sys_printf", {s.addr()});
}

// ------------------------------------------------------------------------------------------------- the module's state

// The configuration module start leaves (0x20C98): where a crash report goes, 14, the report's flags and which extra
// parts a core file holds. A declaration's: /dev_hdd0 (4), 14, 255, none.
struct Config {
    u32 target = 0, fourteen = 0, flags = 0, parts = 0;
};

// The dump's cause (0x20940, 0x338 bytes): what the handler thread records of an exception (PPU, SPU, RSX ... each a
// count and records). A core written by D725F320 alone has none.
struct Cause {
    u64 ppu = 0, spu = 0, rsx = 0, b298 = 0, b2e0 = 0, b328 = 0; // the counts at +0x80, +0x208, +0x250, ...
    u64 pending = 0;                                             // +0x330: a dump to write
};

// The PPU threads and SPU thread groups of the process, as 0x5264 collects them (0x21490, 0x5150 bytes).
struct ThreadCache {
    u32 ppu_count = 0, group_count = 0, spu_count = 0;
    std::vector<u64> ppu;
    struct Group {
        u32 id = 0, count = 0;
        u32 threads[8] = {};
    };
    std::vector<Group> groups;
};

struct State {
    std::mutex mutex;
    bool started = false;     // 0x20930: module start went through the lightweight mutex
    bool declared = false;    // 0x2024C: the executable's declaration (a retail console's crash report)
    bool fsw = false;         // 0x2024B: sys_fsw_connect_event went through
    bool dump_flag = false;   // 0x20488 (0xF44C's argument)
    u32 pid = 0, handler_state = 0; // 0x20934: the process ID and the handler's state
    Config config;
    Cause cause;
    u64 handler_thread = 0;  // 0x20244
    u32 heap = 0;            // 0x20FC8: _LIBLV2COREDUMP_
    u32 data = 0;            // guest memory of the module's own (see kData*)
    u8 app_info[0x100] = {}; // 0x265F8: GAME_APP_INFO (title ID, title, version, SDK version)
    u32 rsx_map = 0;         // 0x2776C: the RSX device 12 mapping (the progress bar)
    // The progress report (0x20538..0x20558).
    u64 progress_total = 0, progress_done = 0;
    u32 progress_mute = 0;
    u8 progress_pct = 0;
    u64 free_space = 0; // 0x21088: the target device's free bytes, as last read
    ThreadCache threads;
};

// The module's guest data: the sys_lwmutex_t of "_l2cMTX" (0x269B8), the event queue's ID word (0x2022C) and the
// handler thread's ID (0x20244, a u64).
constexpr u32 kDataLwmutex = 0x00, kDataQueue = 0x20, kDataThread = 0x28, kDataSize = 0x40;

State& state() { return HleProcess::current()->state<State>(); }

u32 queue_id(PPUContext& ctx, const State& s) { return ps_read32(guest_base(ctx), s.data + kDataQueue); }

// ------------------------------------------------------------------------------------------------- the heap

// 0x2658: a block of `size` bytes when the heap has that much free.
u32 heap_alloc(PPUContext& ctx, State& s, u32 size)
{
    if (!s.heap) return 0;
    const u32 free = static_cast<u32>(lv2(ctx, "_sys_heap_get_total_free_size", {s.heap}));
    if (size > free) return 0;
    return static_cast<u32>(lv2(ctx, "_sys_heap_malloc", {s.heap, size}));
}

// 0x25B4: the same, with at most 32 KB free counted.
u32 heap_alloc_small(PPUContext& ctx, State& s, u32 size)
{
    if (!s.heap) return 0;
    u32 free = static_cast<u32>(lv2(ctx, "_sys_heap_get_total_free_size", {s.heap}));
    if (free > 0x8000) free = 0x8000;
    if (size > free) return 0;
    return static_cast<u32>(lv2(ctx, "_sys_heap_malloc", {s.heap, size}));
}

// 0x2634: n blocks of `each` bytes (0x25B4).
u32 heap_alloc_n(PPUContext& ctx, State& s, u32 each, u32 n)
{
    if (!s.heap) return 0;
    return heap_alloc_small(ctx, s, each * n);
}

// 0x2758
u32 heap_free(PPUContext& ctx, State& s, u32 block)
{
    if (!s.heap) return CELL_ESRCH;
    lv2(ctx, "_sys_heap_free", {s.heap, block});
    return CELL_OK;
}

// ------------------------------------------------------------------------------------------------- the file writer

// The core file's writer (0x5B54, 0xF894; its operations 0xF730, 0xF8FC, 0xF840): a buffer of the heap's (up to
// 32 KB, leaving 32 KB free) in front of sys_fs_write; each write's bytes go to the progress report.
struct Writer {
    Writer(PPUContext& x, State& st, u32 file) : ctx(x), s(st), fd(file) {}
    PPUContext& ctx;
    State& s;
    u32 fd = 0;
    u32 buffer = 0, size = 0, fill = 0;
    bool progress = false;

    // 0xF730: sys_fs_write until all is written; the progress report counts each write.
    s32 write_out(u32 addr, u64 n, u64& written)
    {
        written = 0;
        if (n == 0) return 0;
        u64 done = 0, left = n;
        u8* b = guest_base(ctx);
        StackVar nw(ctx, b, 8);
        while (left != 0) {
            ps_write64(b, nw.addr(), 0);
            const s32 r = sc32(ctx, 803, {fd, static_cast<u32>(addr + done), left, nw.addr()});
            if (r != 0) return r;
            const u64 w = ps_read64(b, nw.addr());
            if (progress) progress_add(w);
            left -= w;
            done += w;
        }
        written = static_cast<u32>(done);
        return 0;
    }

    // 0x5698: write_out until all is out or a write takes nothing.
    s32 write_all(u32 addr, u64 n, u64& total)
    {
        u64 done = 0;
        while (n != 0) {
            u64 w = 0;
            if (const s32 r = write_out(static_cast<u32>(addr + done), n, w); r != 0) return r;
            n -= w;
            if (w == 0) break;
            done += w;
        }
        total = static_cast<u32>(done);
        return 0;
    }

    // 0x57AC: the buffer out; a short write is CELL_EIO.
    s32 flush()
    {
        u64 w = 0;
        if (const s32 r = write_all(buffer, fill, w); r != 0) return r;
        if (fill != w) return static_cast<s32>(CELL_EIO);
        fill = 0;
        return 0;
    }

    // 0x585C: bytes through the buffer.
    s32 put(u32 src, u64 n)
    {
        u8* b = guest_base(ctx);
        u64 at = 0;
        if (fill != 0) {
            u64 room = size - fill;
            while (room < n) {
                n -= room;
                std::memcpy(b + buffer + fill, b + static_cast<u32>(src + at), room);
                at += room;
                u64 w = 0;
                if (const s32 r = write_all(buffer, size, w); r != 0) return r;
                if (w < size) {
                    std::memmove(b + buffer, b + buffer + static_cast<u32>(w), size - w);
                    fill = static_cast<u32>(size - w);
                } else {
                    fill = static_cast<u32>(size - w);
                }
                if (fill == 0) break;
                room = size - fill;
            }
        }
        if (fill == 0 && size < n) {
            u64 w = 0;
            if (const s32 r = write_all(static_cast<u32>(src + at), n, w); r != 0) return r;
            n -= w;
            at += w;
        }
        if (n != 0 && n <= size - fill) {
            std::memcpy(b + buffer + fill, b + static_cast<u32>(src + at), n);
            fill += static_cast<u32>(n);
        }
        return 0;
    }

    // 0x5A6C: zeros, eight at a time (from the module's zeroed data).
    s32 zeros(u64 n)
    {
        u8* b = guest_base(ctx);
        StackVar z(ctx, b, 8);
        while (n != 0) {
            const u64 k = std::min<u64>(n, 8);
            n -= k;
            if (const s32 r = put(z.addr(), k); r != 0) return r;
        }
        return 0;
    }

    // 0xF8FC: the buffer out, then where the file is (into `old`) and a seek to `pos`.
    s32 seek(u64 pos, u64& old)
    {
        if (const s32 r = flush(); r != 0) return r;
        u8* b = guest_base(ctx);
        StackVar cur(ctx, b, 16);
        if (const s32 r = sc32(ctx, 818, {fd, 0, 1, cur.addr()}); r != 0) return r;
        if (const s32 r = sc32(ctx, 818, {fd, pos, 0, cur.addr() + 8}); r != 0) return r;
        old = ps_read64(b, cur.addr());
        return 0;
    }

    void progress_add(u64 n);
};

// ------------------------------------------------------------------------------------------------- the progress report

// 0x11274: each 4 % of the file a dot (and a cell of the bar on the screen, when it is mapped).
void Writer::progress_add(u64 n)
{
    if (s.progress_mute != 0 || n == 0) return;
    if (s.progress_done == s.progress_total) return;
    u64 done = s.progress_done + n;
    if (s.progress_total < done) done = s.progress_total;
    s.progress_done = done;
    const u64 pct = (done * 100 / s.progress_total) & 0xFC;
    u64 step = s.progress_pct;
    if (step < pct) {
        do {
            if (s.rsx_map) PS_LOG_ERROR(kChan, "the progress bar is mapped (RSX device 12): its drawing is not here");
            print(ctx, ".");
            step = (step + 4) & 0xFF;
        } while (step < pct);
        s.progress_pct = static_cast<u8>(pct);
    }
}

// 0x1143C: the report's start: the message, the screen's mode, the bar (RSX device 12, sys_rsx_device_map); an error
// when the screen cannot be read or mapped.
s32 progress_begin(PPUContext& ctx, State& s)
{
    s.progress_mute = 0;
    s.progress_pct = 0;
    s.progress_done = 0;
    s.progress_total = 0;
    print(ctx, "Generating Core Dump File ");
    u8* b = guest_base(ctx);
    StackVar state_buf(ctx, b, 16), res(ctx, b, 16);
    s32 r = static_cast<s32>(call_nid(ctx, "cellSysutil", 0x887572D5, {0, 0, state_buf.addr()})); // GetState
    if (r != 0) return r;
    const u8 resolution = ps_read8(b, state_buf.addr() + 8); // CellVideoOutState.displayMode.resolutionId
    r = static_cast<s32>(call_nid(ctx, "cellSysutil", 0xE558748D, {resolution, res.addr()})); // GetResolution
    if (r != 0) return r;
    if (static_cast<u8>(resolution - 4) >= 2 && static_cast<u8>(resolution - 1) > 1 && resolution != 13 &&
        resolution != 12 && resolution != 11 && resolution != 10)
        return -1;
    // 0x182E8: the bar's memory.
    StackVar m(ctx, b, 16);
    s.rsx_map = 0;
    r = sc32(ctx, 675, {m.addr(), m.addr() + 8, 12});
    if (r != 0) return r;
    s.rsx_map = static_cast<u32>(ps_read64(b, m.addr()));
    PS_LOG_ERROR(kChan, "RSX device 12 mapped at {:#x}: the progress bar's drawing is not here", s.rsx_map);
    return 0;
}

// 0x113D4: " Done." or the error; the bar's memory unmapped (0x18340).
void progress_end(PPUContext& ctx, State& s, s32 result)
{
    if (result == 0) print(ctx, " Done.\n");
    else print(ctx, std::format(" Failed. (error: 0x{:08x})\n", static_cast<u32>(result)));
    if (s.rsx_map != 0 && sc32(ctx, 676, {12}) == 0) s.rsx_map = 0;
}

// ------------------------------------------------------------------------------------------------- the core file

// One part of the core file: its count of program headers (0 or 1) and bytes (sizing, 0xFFD8), its program header
// (0xFCF8's first pass) and its note (the second pass; `end` closes it, 0xFED0).
struct Core;
struct Part {
    virtual ~Part() = default;
    virtual void size(Core& c, u64& count, u64& bytes) = 0;
    virtual s32 header(Core& c, u64 offset) = 0;
    virtual s32 data(Core& c) = 0;
};

struct Core {
    Core(PPUContext& x, State& st) : ctx(x), s(st) {}
    PPUContext& ctx;
    State& s;
    Writer* w = nullptr;
    u16 phnum = 0;                 // 0x277F0: the program headers written so far
    std::vector<Part*> parts;      // 0x277F8
    std::vector<u64> counts, sizes; // 0x26700, 0x26848
    u64 layout_phnum = 0, layout_end = 0x40, layout_phdr_end = 0x40; // 0x277D8
    u32 fit = 0;                   // 0x27A74: the parts that fit

    u8* b() const { return guest_base(ctx); }

    // 0x4864: a note's 0x30 bytes: its name's size (16), its descriptor's (the part's bytes less 0x1C), its type, the
    // name ("%s" into 16 bytes), then the entries' size and count.
    s32 note(std::string_view name, u32 type, u32 bytes, u32 count, u32 entry_size)
    {
        StackVar h(ctx, b(), 0x30);
        u8* p = b() + h.addr();
        ps_write32(b(), h.addr(), 16);
        ps_write32(b(), h.addr() + 4, bytes - 0x1C);
        ps_write32(b(), h.addr() + 8, type);
        std::memcpy(p + 12, name.data(), std::min<usize>(name.size(), 15));
        ps_write32(b(), h.addr() + 0x1C, entry_size);
        ps_write32(b(), h.addr() + 0x20, count);
        return w->put(h.addr(), 0x30);
    }

    // 0xFC8C (0x41C8, 0x3E5C): a PT_NOTE program header at `offset` of `bytes` (none for an empty part).
    s32 program_header(u64 offset, u64 bytes)
    {
        if (bytes == 0) return 0;
        StackVar h(ctx, b(), 0x38);
        ps_write32(b(), h.addr(), 4);       // PT_NOTE
        ps_write32(b(), h.addr() + 4, 4);   // PF_R
        ps_write64(b(), h.addr() + 8, offset);
        ps_write64(b(), h.addr() + 0x20, bytes);
        ps_write64(b(), h.addr() + 0x30, 4);
        return w->put(h.addr(), 0x38);
    }

    // 0xFED0: a part's end: the header's program header count, one more, written in place (when the file can seek;
    // the progress report not counting it).
    s32 end()
    {
        phnum++;
        u64 old = 0;
        s32 r = w->seek(0x38, old);
        if (r == static_cast<s32>(CELL_ENOTSUP)) return 0;
        if (r != 0) return r;
        s.progress_mute = 1;
        StackVar n(ctx, b(), 8);
        ps_write16(b(), n.addr(), phnum);
        r = w->put(n.addr(), 2);
        if (r == 0) {
            u64 back = 0;
            r = w->seek(old, back);
            s.progress_mute = 0;
            return r;
        }
        return r;
    }
};

// ---- SYSTEM_INFO (0x125CC: 0x8E64, 0xF148, 0xE724): the console's OpenPSID and the system software's version.
struct SystemInfo final : Part {
    bool present = false;
    u32 bytes = 0, count = 0;
    void size(Core&, u64& n, u64& sz) override
    {
        n = 1;
        count = 1;
        bytes = 0x1D0;
        sz = 0x1D0;
        present = true;
    }
    s32 header(Core& c, u64 offset) override { return c.program_header(offset, bytes); }
    s32 data(Core& c) override
    {
        if (!present) return 0;
        const u32 e = heap_alloc_small(c.ctx, c.s, 0x1A0);
        if (!e) {
            s32 r = c.note("SYSTEM_INFO", 0x1000, bytes, 0, 0x1A0);
            if (r == 0) r = c.w->zeros(0x1A0);
            if (r == 0) r = c.end();
            return r;
        }
        s32 r = c.note("SYSTEM_INFO", 0x1000, bytes, count, 0x1A0);
        if (r == 0) {
            u8* b = c.b();
            StackVar psid(c.ctx, b, 16);
            u64 hi = 0, lo = 0;
            if (sc32(c.ctx, 872, {psid.addr()}) == 0) {
                hi = ps_read64(b, psid.addr());
                lo = ps_read64(b, psid.addr() + 8);
            }
            std::memset(b + e, 0, 0x1A0);
            ps_write64(b, e, hi);
            ps_write64(b, e + 8, lo);
            // The system software's version as liblv2coredump was built: 0x00000007 0x00492000.
            ps_write8(b, e + 0x13, 7);
            ps_write8(b, e + 0x15, 0x49);
            ps_write8(b, e + 0x16, 0x20);
            r = c.w->put(e, 0x1A0);
            if (r == 0) r = c.end();
        }
        heap_free(c.ctx, c.s, e);
        return r;
    }
};

// ---- DUMP_CAUSE_INFO (0x11E3C: 0x7880, 0xEC44, 0x9CC0): the exception records the handler thread kept (0x68 bytes
// each).
struct DumpCause final : Part {
    bool present = false;
    u32 bytes = 0, entries = 0;
    void size(Core& c, u64& n, u64& sz) override
    {
        present = true;
        const Cause& k = c.s.cause;
        const u64 total = k.ppu + k.spu + k.b2e0 + k.rsx + k.b298 + k.b328;
        n = 1;
        entries = static_cast<u32>(total);
        const u64 v = total * 0x68 + 0x30;
        bytes = static_cast<u32>(v);
        sz = v & 0xFFFFFFFF;
    }
    s32 header(Core& c, u64 offset) override { return c.program_header(offset, bytes); }
    s32 data(Core& c) override
    {
        if (!present) return 0;
        s32 r = c.note("DUMP_CAUSE_INFO", 0x1006, bytes, entries, 0x68);
        if (r != 0) return r;
        if (entries != 0) {
            PS_LOG_ERROR(kChan, "a dump cause's records are not written here yet: {} zero records", entries);
            for (u32 i = 0; i < entries && r == 0; i++) r = c.w->zeros(0x68);
            if (r != 0) return r;
        }
        return c.end();
    }
};

// ---- PROCESS_INFO (0x12084: 0x8400, 0xEE54, 0xC518)
struct ProcessInfo final : Part {
    bool present = false;
    u32 bytes = 0, count = 0;
    void size(Core&, u64& n, u64& sz) override
    {
        n = 1;
        count = 1;
        bytes = 0x36C;
        sz = 0x36C;
        present = true;
    }
    s32 header(Core& c, u64 offset) override { return c.program_header(offset, bytes); }
    s32 data(Core& c) override
    {
        if (!present) return 0;
        const u32 pid = c.s.pid;
        const u32 e = heap_alloc_small(c.ctx, c.s, 0x33C);
        if (!e) {
            s32 r = c.note("PROCESS_INFO", 0x1002, bytes, 0, 0x33C);
            if (r == 0) r = c.w->zeros(0x33C);
            if (r == 0) r = c.end();
            return r;
        }
        u8* b = c.b();
        // sys_dbg_get_process_info into the stack: the structure (0x38 bytes) and its name's 512 bytes.
        StackVar info(c.ctx, b, 0x38 + 0x200);
        const u32 name = info.addr() + 0x38;
        ps_write32(b, info.addr() + 0x18, name);
        sc(c.ctx, 914, {pid, info.addr()});
        std::memset(b + e, 0, 0x33C);
        std::memcpy(b + e, b + info.addr() + 4, 4);
        ps_write32(b, e + 4, c.s.threads.ppu_count);
        ps_write32(b, e + 8, c.s.threads.group_count);
        std::memcpy(b + e + 12, b + info.addr() + 0x10, 8);
        ps_write32(b, e + 24, ps_read32(b, info.addr() + 0x1C));
        std::memcpy(b + e + 0x1C, b + name, 0x200);
        // The PPU GUID (sys_process_get_ppu_guid), when its 32 bytes can be read.
        const u32 guid = static_cast<u32>(sc(c.ctx, 31, {}));
        if (readable_range(c, guid, 0x20)) std::memcpy(b + e + 0x21C, b + guid, 0x20);
        ps_write32(b, e + 0x23C, pid);
        s32 r = c.note("PROCESS_INFO", 0x1002, bytes, count, 0x33C);
        if (r == 0) r = c.w->put(e, 0x33C);
        if (r == 0) r = c.end();
        heap_free(c.ctx, c.s, e);
        return r;
    }

    // 0x3A00: every page of [addr, addr + size) readable by a PPU thread (sys_memory_get_page_attribute).
    static bool readable_range(Core& c, u32 addr, u32 size)
    {
        const u32 end = addr + size;
        if (!(addr < end)) return true;
        u8* b = c.b();
        StackVar attr(c.ctx, b, 0x18);
        u32 a = addr;
        if (sc32(c.ctx, 351, {a, attr.addr()}) != 0) return false;
        for (;;) {
            if (!(ps_read64(b, attr.addr() + 8) & 8)) return false;
            const u32 page = ps_read32(b, attr.addr() + 0x10);
            a = page + (a / page) * page;
            if (end <= a) break;
            if (sc32(c.ctx, 351, {a, attr.addr()}) != 0) return false;
        }
        return true;
    }
};

// ---- GAME_APP_INFO (0x7404: 0x743C, 0xEB84, 0x92E8): module start's title ID, title, version and SDK version.
struct GameAppInfo final : Part {
    bool present = false;
    u32 bytes = 0, count = 0;
    void size(Core&, u64& n, u64& sz) override
    {
        n = 1;
        count = 1;
        bytes = 0x130;
        sz = 0x130;
        present = true;
    }
    s32 header(Core& c, u64 offset) override { return c.program_header(offset, bytes); }
    s32 data(Core& c) override
    {
        if (!present) return 0;
        s32 r = c.note("GAME_APP_INFO", 0x1017, bytes, count, 0x100);
        if (r != 0) return r;
        u8* b = c.b();
        StackVar info(c.ctx, b, 0x100);
        std::memcpy(b + info.addr(), c.s.app_info, 0x100);
        r = c.w->put(info.addr(), 0x100);
        if (r != 0) return r;
        return c.end();
    }
};

// ---- the process's memory (0x120BC: 0x8438, 0xEE84, 0xC858): PT_LOAD program headers and the pages, from the
// regions the handler thread saved (sys_dbg_vm_get_info). D725F320 has none: the saved list's file is not open (its
// first seek fails) and a retail console's lv2 has no sys_dbg_vm_get_info.
struct Memory final : Part {
    u32 count = 0;
    void size(Core& c, u64& n, u64& sz) override
    {
        count = 0;
        u8* b = c.b();
        StackVar pos(c.ctx, b, 8);
        sc(c.ctx, 818, {0, 0, 0, pos.addr()}); // the list's file: not open
        n = count;
        sz = 0;
    }
    s32 header(Core&, u64) override { return 0; }
    s32 data(Core&) override { return 0; }
};

// 0x5264: the process's PPU threads and SPU thread groups (sys_dbg_get_thread_list, sys_dbg_get_thread_info).
void collect_threads(PPUContext& ctx, State& s, u32 pid)
{
    ThreadCache& tc = s.threads;
    tc = {};
    u8* b = guest_base(ctx);
    StackVar v(ctx, b, 0x60);
    const u32 cap = v.addr(), total = v.addr() + 8;
    ps_write64(b, cap, 0);
    ps_write64(b, total, 0);
    if (sc32(ctx, 909, {pid, 0, cap, total}) != 0) return;
    const u32 list = heap_alloc_n(ctx, s, 0x10, ps_read32(b, total + 4));
    if (!list) return;
    ps_write64(b, cap, ps_read64(b, total));
    if (sc32(ctx, 909, {pid, list, cap, total}) == 0) {
        const u64 n = ps_read64(b, cap);
        for (u64 i = 0; i < n; i++) {
            const u32 at = static_cast<u32>(list + 0x10 * i);
            const u32 type = ps_read32(b, at);
            if (type == 0) {
                tc.ppu.push_back(ps_read64(b, at + 8));
                tc.ppu_count++;
            } else if (type == 2) {
                ThreadCache::Group g;
                g.id = ps_read32(b, at + 8);
                // sys_dbg_get_thread_info of the group: its threads' IDs, eight at most.
                const u32 req = v.addr() + 0x10, out = v.addr() + 0x20, ids = v.addr() + 0x3C;
                std::memset(b + out, 0, 0x1C);
                ps_write32(b, req, 2);
                ps_write32(b, req + 8, g.id);
                ps_write32(b, out + 0x10, 8);
                ps_write32(b, out + 0x0C, 0);
                ps_write32(b, out + 0x14, 0);
                ps_write32(b, out + 0x18, ids);
                if (sc32(ctx, 910, {req, out}) == 0) {
                    const u32 k = ps_read32(b, out + 0x10);
                    for (u32 j = 0; j < k && j < 8; j++) g.threads[j] = ps_read32(b, ids + 4 * j);
                    g.count = k;
                    tc.spu_count += k;
                }
                tc.groups.push_back(g);
                tc.group_count++;
            }
        }
    }
    heap_free(ctx, s, list);
}

// 0xF9E8: where every part goes; the parts that fit on the target's device (sys_fs_disk_free: more than 10 KB free).
s32 lay_out(Core& c, u64 limit, u64& total)
{
    total = 0;
    c.fit = 0;
    c.layout_phdr_end = 0x40;
    c.layout_phnum = 0;
    c.layout_end = 0x40;
    const u32 target = c.s.config.target;
    std::string_view device;
    switch (target) {
    case 1:
    case 2: c.s.free_space = ~u64{0}; break;
    case 4: device = "/dev_hdd0"; break;
    case 8: device = "/dev_hdd1"; break;
    case 16: device = "/dev_ms"; break;
    case 32: device = "/dev_usb"; break;
    default: break;
    }
    if (!device.empty()) {
        u8* b = c.b();
        StackVar v(c.ctx, b, 0x20);
        std::memcpy(b + v.addr(), device.data(), device.size());
        const s32 r = sc32(c.ctx, 840, {v.addr(), v.addr() + 0x10, v.addr() + 0x18});
        if (r != 0) return r;
        c.s.free_space = ps_read64(b, v.addr() + 0x18);
    }
    if (target != 1 && target != 2 && c.s.free_space <= 0x2800) return static_cast<s32>(CELL_ENOSPC);
    if (!c.parts.empty()) {
        u64 end = c.layout_end, phnum = c.layout_phnum, phdr_end = c.layout_phdr_end;
        u32 fit = 0;
        for (usize i = 0; i < c.parts.size(); i++) {
            const u64 next = end + c.sizes[i] + c.counts[i] * 0x38;
            if (next > c.s.free_space) break;
            if (limit != 0 && next > limit) break;
            end = next;
            phnum += c.counts[i];
            phdr_end += c.counts[i] * 0x38;
            fit++;
        }
        c.fit = fit;
        c.layout_end = end;
        c.layout_phnum = phnum;
        c.layout_phdr_end = phdr_end;
    }
    total = c.layout_end;
    return 0;
}

// 0xFCF8: the ELF header (its program header count 0, filled in as each part ends), the parts' program headers, then
// the parts.
s32 write_core(Core& c)
{
    u8* b = c.b();
    StackVar h(c.ctx, b, 0x40);
    u8* p = b + h.addr();
    const u8 ident[16] = {0x7F, 'E', 'L', 'F', 2, 2, 1, 0x66, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(p, ident, 16);
    ps_write16(b, h.addr() + 0x10, 4);    // ET_CORE
    ps_write16(b, h.addr() + 0x12, 0x15); // EM_PPC64
    ps_write32(b, h.addr() + 0x14, 1);
    ps_write64(b, h.addr() + 0x20, 0x40); // e_phoff
    ps_write16(b, h.addr() + 0x34, 0x40); // e_ehsize
    ps_write16(b, h.addr() + 0x36, 0x38); // e_phentsize
    ps_write16(b, h.addr() + 0x38, 0);    // e_phnum: the file can seek
    ps_write16(b, h.addr() + 0x3A, 0x40); // e_shentsize
    c.phnum = 0;
    s32 r = c.w->put(h.addr(), 0x40);
    if (r != 0 || c.fit == 0) return r;
    u64 offset = c.layout_phdr_end;
    for (u32 i = 0; i < c.fit; i++) {
        r = c.parts[i]->header(c, offset);
        if (r != 0) return r;
        offset += c.sizes[i];
    }
    for (u32 i = 0; i < c.fit; i++) {
        r = c.parts[i]->data(c);
        if (r != 0) return r;
    }
    return 0;
}

// 0x10118: the core file into `fd`: SYSTEM_INFO, DUMP_CAUSE_INFO, PROCESS_INFO, GAME_APP_INFO and the memory part, the
// parts of the declaration's configuration (a debugging console's settings would add parts in between).
s32 write_core_file(PPUContext& ctx, State& s, u32 fd)
{
    SystemInfo system_info;
    DumpCause cause;
    ProcessInfo process;
    GameAppInfo app;
    Memory memory;
    Core c{ctx, s};
    c.parts.push_back(&system_info);
    c.parts.push_back(&cause);
    c.parts.push_back(&process);
    c.parts.push_back(&app);
    c.parts.push_back(&memory);

    progress_begin(ctx, s);
    // 0x3724: the memory kinds to dump, from sys_dbg_vm_get_info (none on a retail console's lv2).
    {
        u8* b = guest_base(ctx);
        StackVar n(ctx, b, 8);
        sc(ctx, 954, {s.pid, 0, n.addr()});
    }
    // 0xFFD8: every part's size (after the thread list).
    c.counts.assign(c.parts.size(), 0);
    c.sizes.assign(c.parts.size(), 0);
    collect_threads(ctx, s, s.pid);
    for (usize i = 0; i < c.parts.size(); i++) c.parts[i]->size(c, c.counts[i], c.sizes[i]);
    u64 total = 0;
    s32 r = lay_out(c, 0, total);
    bool ok = false;
    if (r == 0) {
        s.progress_total = total;
        Writer w{ctx, s, fd};
        // 0x5B54: the buffer, 32 KB or what the heap has beyond 32 KB.
        u32 size = 0x8000;
        if (s.heap) {
            const u32 free = static_cast<u32>(lv2(ctx, "_sys_heap_get_total_free_size", {s.heap}));
            if (free < size + 0x8000) size = free > 0x8000 ? free - 0x8000 : 0;
        }
        w.size = size;
        w.buffer = size ? heap_alloc(ctx, s, size) : 0;
        if (!w.buffer) {
            r = static_cast<s32>(CELL_ENOMEM);
        } else {
            std::memset(guest_base(ctx) + w.buffer, 0, size);
            w.progress = true;
            c.w = &w;
            r = write_core(c);
            // 0xF848: the buffer out and freed; its error is the answer's (the report not completed).
            const s32 f = w.flush();
            heap_free(ctx, s, w.buffer);
            if (f != 0) r = f;
            else ok = r == 0;
        }
    }
    if (ok) {
        // 0x1138C: the report to 100 %.
        if (s.progress_done < s.progress_total) {
            Writer w{ctx, s, fd};
            w.progress_add(s.progress_total - s.progress_done);
        }
    }
    progress_end(ctx, s, r);
    return r;
}

// ------------------------------------------------------------------------------------------------- module start

// 0x90A8: GAME_APP_INFO's bytes: the title ID (10 bytes at 0), the title (0x80 at 10), the version (6 at 0x8A; "00.00"
// until read), the SDK version (at 0x90); the parameters from libsysutil_game, loaded for the purpose
// (cellGameBootCheck, cellGameGetParamString, cellGameContentPermit), then unloaded.
void read_app_info(PPUContext& ctx, State& s)
{
    u8* b = guest_base(ctx);
    StackVar v(ctx, b, 0x2A0);
    const u32 info = v.addr();
    std::memcpy(b + info + 0x8A, "00.00", 6);
    sc(ctx, 25, {1, info + 0x90}); // sys_process_get_sdk_version
    const u32 path = info + 0x100, res = info + 0x140, type = info + 0x150, attr = info + 0x154, size = info + 0x158,
              dir = info + 0x170, content = info + 0x190, usrdir = info + 0x210;
    const char* sprx = "/dev_flash/sys/external/libsysutil_game.sprx";
    std::memcpy(b + path, sprx, std::strlen(sprx) + 1);
    const s32 id = static_cast<s32>(lv2(ctx, "sys_prx_load_module", {path, 0, 0}));
    if (id > 0) {
        if (static_cast<s32>(lv2(ctx, "sys_prx_start_module", {static_cast<u32>(id), 0, 0, res, 0, 0})) == 0) {
            std::memset(b + size, 0, 0xC);
            if (call_nid(ctx, "cellGame", 0xF52639EA, {type, attr, size, dir}) == 0) {
                call_nid(ctx, "cellGame", 0x3A5D726A, {100, info, 10});
                call_nid(ctx, "cellGame", 0x3A5D726A, {1, info + 10, 0x80});
                call_nid(ctx, "cellGame", 0x3A5D726A, {0x6A, info + 0x8A, 6});
                call_nid(ctx, "cellGame", 0x70ACEC67, {content, usrdir});
            }
            lv2(ctx, "sys_prx_stop_module", {static_cast<u32>(id), 0, 0, res + 4, 0, 0});
        }
        lv2(ctx, "sys_prx_unload_module", {static_cast<u32>(id), 0, 0});
    }
    std::memcpy(s.app_info, b + info, 0x100);
}

// 0xAF8: what start made, undone.
void undo_start(PPUContext& ctx, State& s)
{
    if (s.fsw) sc(ctx, 634, {queue_id(ctx, s)});
    if (queue_id(ctx, s) != 0) sc(ctx, 129, {queue_id(ctx, s), 1});
    if (s.heap) {
        lv2(ctx, "_sys_heap_delete_heap", {s.heap});
        s.heap = 0;
    }
    ps_write64(guest_base(ctx), s.data + kDataThread, 0);
    s.handler_thread = 0;
    s.fsw = false;
    ps_write32(guest_base(ctx), s.data + kDataQueue, 0);
    s.pid = s.handler_state = 0;
    s.config = {};
}

// 0xBA8: the heap, the event queue registered as the exception handler, the title's information, the thread; the
// declaration's configuration.
u32 set_up(PPUContext& ctx, State& s, u32 pid)
{
    u8* b = guest_base(ctx);
    if (queue_id(ctx, s) != 0 || ps_read64(b, s.data + kDataThread) != 0) return CELL_EAGAIN_ALREADY;
    ps_write32(b, s.data + kDataQueue, 0);
    ps_write64(b, s.data + kDataThread, 0);
    s.fsw = false;
    s.declared = false;
    s.pid = s.handler_state = 0;
    s.config = {};
    s.cause = {};
    // 0x27FC: the heap.
    {
        StackVar name(ctx, b, 0x20);
        std::memcpy(b + name.addr(), "_LIBLV2COREDUMP_", 17);
        s.heap = static_cast<u32>(lv2(ctx, "_sys_heap_create_heap", {name.addr(), 0x10000, 0x110000, 0}));
    }
    if (!s.heap) {
        undo_start(ctx, s);
        return CELL_ENOMEM;
    }
    // The queue: priority order, a PPU queue, "_l2cDBG", 24 events.
    {
        StackVar attr(ctx, b, 0x10);
        ps_write32(b, attr.addr(), 2);
        ps_write32(b, attr.addr() + 4, 1);
        std::memcpy(b + attr.addr() + 8, "_l2cDBG", 8);
        const s32 r = sc32(ctx, 128, {s.data + kDataQueue, attr.addr(), 0, 24});
        if (r != 0) {
            undo_start(ctx, s);
            return static_cast<u32>(r);
        }
    }
    if (const s32 r = sc32(ctx, 944, {queue_id(ctx, s)}); r != 0) {
        undo_start(ctx, s);
        return static_cast<u32>(r);
    }
    s.pid = pid;
    s.config = {4, 14, 255, 0};
    s.declared = true;
    // sys_fsw_connect_event: a retail console's lv2 has none.
    if (sc32(ctx, 633, {queue_id(ctx, s)}) == 0) s.fsw = true;
    read_app_info(ctx, s);
    // The handler thread: priority 1001, a 16 KB stack.
    {
        HleProcess& p = *HleProcess::current();
        const u32 entry = p.link_function(kModule, recomp::nid_of("_pspocket_lv2coredump_handler"),
            "_pspocket_lv2coredump_handler");
        StackVar name(ctx, b, 0x20);
        std::memcpy(b + name.addr(), "__COREDUMP_HANDLER__", 21);
        const s32 r = static_cast<s32>(
            lv2(ctx, "sys_ppu_thread_create", {s.data + kDataThread, entry, 0, 1001, 0x4000, 0, name.addr()}));
        if (r != 0) {
            undo_start(ctx, s);
            return static_cast<u32>(r);
        }
    }
    s.handler_thread = ps_read64(b, s.data + kDataThread);
    // sys_dbg_disable_floating_point_enabled_exception: a retail console's lv2 has none.
    u32 st = 1;
    if (static_cast<u32>(sc(ctx, 956, {0xFFFFFFFFu})) != 0x80010003) {
        const s32 r = sc32(ctx, 956, {s.handler_thread});
        st = r != 0 ? 3 : 2;
    }
    s.handler_state = st;
    return CELL_OK;
}

// 0xF44C: the user log areas' lightweight mutex ("_l2cMTX") and table.
s32 set_up_log(PPUContext& ctx, State& s, bool flag)
{
    s.dump_flag = flag;
    u8* b = guest_base(ctx);
    StackVar attr(ctx, b, 0x10);
    ps_write32(b, attr.addr(), 2);
    ps_write32(b, attr.addr() + 4, 0x20);
    std::memcpy(b + attr.addr() + 8, "_l2cMTX", 8);
    const s32 r = static_cast<s32>(lv2(ctx, "sys_lwmutex_create", {s.data + kDataLwmutex, attr.addr()}));
    return r;
}

} // namespace

// The module's start (0x50): the process's declaration (its _sys_process_get_paramsfo flag 0x10 and address), checked,
// then 0xF44C and 0xBA8. A debugging console's crash dump settings (+0x20 there) would configure more; pspocket's lv2
// reports none.
s32 lv2coredump_start(PPUContext& ctx, u64 args, u64 argp)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    u8* b = guest_base(ctx);
    if (!s.data) {
        s.data = HleProcess::current()->main_memory().alloc(kDataSize, 0x10);
        if (!s.data) PS_FATAL(kChan, "no main memory for liblv2coredump's data");
        std::memset(b + s.data, 0, kDataSize);
    }
    StackVar sfo(ctx, b, 0x40);
    const auto stop = [&]() -> s32 {
        // 0x8: the module's exit (the lightweight mutex, when start made it).
        if (s.started) {
            lv2(ctx, "sys_lwmutex_destroy", {s.data + kDataLwmutex});
            s.started = false;
        }
        return 0;
    };
    if (args == 0 || argp == 0) {
        if (lv2(ctx, "sys_process_get_paramsfo", {sfo.addr()}) != 0) return stop();
    } else {
        if (args != 0x40) return stop();
        std::memcpy(b + sfo.addr(), b + static_cast<u32>(argp), 0x40);
    }
    if (!(ps_read64(b, sfo.addr() + 0x10) & 0x10)) return stop();
    if (ps_read64(b, sfo.addr() + 0x20) != 0)
        PS_LOG_ERROR(kChan, "a debugging console's crash dump settings ({:#x}): not taken (pspocket's console is a "
                            "retail one)",
            ps_read64(b, sfo.addr() + 0x20));
    const u64 decl = ps_read64(b, sfo.addr() + 0x38);
    if (decl == 0) return stop();
    const u32 d = static_cast<u32>(decl);
    StackVar attr(ctx, b, 0x18);
    if (sc32(ctx, 351, {d, attr.addr()}) != 0 || !(ps_read64(b, attr.addr() + 8) & 8)) {
        print(ctx, "[Error] coredump function couldn't be enabled due to invalid SYS_PROCESS_CRASH_DUMP_ENABLED "
                   "declaration.\n");
        return stop();
    }
    if (ps_read32(b, d) != 0x20 || ps_read32(b, d + 4) != 0x64756D70 || ps_read32(b, d + 8) != 1 ||
        ps_read32(b, d + 12) != 0 || ps_read64(b, d + 16) != 0 || ps_read64(b, d + 24) != 0) {
        print(ctx, "[Error] coredump function couldn't be enabled due to SYS_PROCESS_CRASH_DUMP_ENABLED "
                   "declaration with invalid arguments.\n");
        return stop();
    }
    if (set_up_log(ctx, s, true) != 0) return stop();
    s.started = true;
    const u32 pid = static_cast<u32>(sc(ctx, 1, {}));
    return static_cast<s32>(set_up(ctx, s, pid));
}

namespace {

// sys_lv2coredump_D725F320(fd): the core file into `fd` (0xA84: 0x10118 with the dump's cause and configuration).
s32 sys_lv2coredump_D725F320(PPUContext& ctx, u32 fd)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    return write_core_file(ctx, s, fd);
}

// The handler thread (0x1584): its process ID, then the queue's events; the queue gone, the module's objects with it.
// pspocket's lv2 sends it no PPU exception yet, and an event (the queue's last, from sys_dbg_finalize_ppu_exception_handler
// or the unnamed 988; one a game sends with 959 or 972) is logged and left: the crash report it can lead to is not here.
void _pspocket_lv2coredump_handler(PPUContext& ctx, u64)
{
    State& s = state();
    sc(ctx, 1, {});
    u8* b = guest_base(ctx);
    StackVar ev(ctx, b, 0x20);
    for (;;) {
        u32 q = 0;
        {
            std::lock_guard lock(s.mutex);
            q = queue_id(ctx, s);
        }
        // sys_event_queue_receive: the event in r4..r7.
        u64 saved[10];
        for (u32 k = 0; k < 10; k++) saved[k] = ctx.gpr[3 + k];
        ctx.gpr[3] = q;
        ctx.gpr[4] = ev.addr();
        ctx.gpr[5] = 0;
        ctx.gpr[11] = 130;
        thread_of(ctx).kernel.syscall(ctx);
        const s32 r = static_cast<s32>(ctx.gpr[3]);
        const u64 source = ctx.gpr[4], d1 = ctx.gpr[5], d2 = ctx.gpr[6], d3 = ctx.gpr[7];
        for (u32 k = 0; k < 10; k++) ctx.gpr[3 + k] = saved[k];
        if (r != 0) {
            std::lock_guard lock(s.mutex);
            undo_start(ctx, s);
            lv2(ctx, "sys_ppu_thread_exit", {0});
            return;
        }
        u32 st = 0;
        bool declared = false;
        {
            std::lock_guard lock(s.mutex);
            st = s.handler_state;
            declared = s.declared;
        }
        if (st - 1 > 1 || (st == 1 && !declared)) {
            lv2(ctx, "sys_ppu_thread_exit", {0});
            return;
        }
        PS_LOG_WARN(kChan, "the crash dump handler's queue: event {:#x} {:#x} {:#x} {:#x}: no exception recorded here",
            source, d1, d2, d3);
    }
}

} // namespace

void register_sys_lv2coredump(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0xD725F320, sys_lv2coredump_D725F320),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_lv2coredump_handler"), _pspocket_lv2coredump_handler),
    });
}

} // namespace pspocket::hle
