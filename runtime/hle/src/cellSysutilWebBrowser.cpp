// cellSysutil's web browser and web component (D-534): libsysutil's code for them, read from libsysutil.sprx (4.92) and
// compared with it call for call (tests/hle/test_sysutil_cxml_compare.cpp). RPCS3 had only the names (its
// cellWebBrowser.cpp: stubs, Estimate2 answering 1 MB, Initialize and Shutdown sending their system events at once).
//
// The browser is the system software's. libsysutil only fills the game's configs, estimates the memory the browser
// needs, keeps the hooks the game gives (eleven {callback, userdata} pairs in its data), checks the memory container
// the game gave cellWebBrowserInitialize, and sends the system its requests on channel 0 (packet ID 0x10E, documents
// with the magic "WBRW": initialize, finalize, createFullBrowser, destroyFullBrowser, activate, deactivate,
// navigateBrowser, pointer, wakeupWithGameExit, GetUsrdataOnGameExit), reading the reply where one comes. The
// system's events (status changes, the hooks' calls) come back through the callback dispatcher libsysutil registers for
// 0x112, which reads the event's data on channel 1. pspocket's console has no system software: a request is built
// exactly as libsysutil builds it and its send answers 0x8002b007 (the channels are closed, sysutil_cxml.hpp
// packets_start), printed and answered as libsysutil does, and no event ever comes back. libsysutil's slips are kept:
// a failed create leaves the browser marked made (only the system's "unloaded" event clears it), so a second create
// answers 0x8002b903; the memory checks read the container's size and ignore lv2's answer (with a bad container they
// compare what the stack held: pspocket's is zeros, so the check fails).

#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <mutex>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"
#include "sysutil_cxml.hpp"

namespace pspocket::hle::sysutil {

namespace {

constexpr std::string_view kChan = "cellSysutil";
constexpr std::string_view kFile = "sysutil_webbrowser.cpp";

enum : u32 {
    CELL_WEBBROWSER_ERROR_NOMEM = 0x8002b901,
    CELL_WEBBROWSER_ERROR_INVALID = 0x8002b902,
    CELL_WEBBROWSER_ERROR_BUSY = 0x8002b903,
    CELL_EFAULT = 0x8001000D,
};
constexpr u32 kConfig2Version = 0x20000, kConfig2Magic = 0xDEADBEEF;
constexpr u32 kPacketId = 0x10E;
constexpr u32 kDispatcherEvent = 0x112;
constexpr u32 kNidRegisterDispatcher = 0x886D0747, kNidUnregisterDispatcher = 0x40C7538E;
constexpr u32 kNidMemoryContainerGetSize = 343;

constexpr s32 err(u32 code) { return static_cast<s32>(code); }

// PPC fctiwz: toward zero, saturating, NaN the most negative.
s32 fctiwz(double v)
{
    if (std::isnan(v) || v <= -2147483649.0) return std::numeric_limits<s32>::min();
    if (v >= 2147483648.0) return std::numeric_limits<s32>::max();
    return static_cast<s32>(v);
}
s32 fctiwz(float v) { return fctiwz(static_cast<double>(v)); }

// cellSysutil's own export, as libsysutil calls it (pspocket's, through the registry; r3..r10 kept).
u64 sysutil_call(PPUContext& ctx, u32 nid, std::initializer_list<u64> args)
{
    const Function* f = registry().find("cellSysutil", nid);
    if (!f) PS_FATAL(kChan, "cellSysutil {:#010x} is not pspocket's", nid);
    u64 saved[8];
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 0;
    for (const u64 a : args) ctx.gpr[3 + i++] = a;
    f->fn(ctx, guest_base(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

s32 register_dispatcher(PPUContext& ctx, u32 code, u32 func)
{
    return static_cast<s32>(static_cast<u32>(sysutil_call(ctx, kNidRegisterDispatcher, {code, func})));
}

// The descriptor of libsysutil's handler of the system's web browser events (pspocket's: cellSysutil's internal
// function, linked here when the loader has not).
constexpr std::string_view kEventHandler = "cellSysutil_WebBrowserSystemEvent";

u32 event_handler()
{
    HleProcess* p = HleProcess::current();
    const u32 nid = recomp::nid_of(kEventHandler);
    if (const u32 d = p->descriptor_of("cellSysutil", nid)) return d;
    return p->link_function("cellSysutil", nid, kEventHandler);
}

u32 hook_cb(Mem& m, u32 g, u32 n) { return m.r32(g + glob::web_hooks + 8 * n); }

// libsysutil's hooks cleared: all of them (FUN_00014924), or all only when the config gives none (FUN_00014954).
void clear_hooks(Mem& m, u32 g)
{
    for (u32 n = 0; n < 11; n++) {
        m.w32(g + glob::web_hooks + 8 * n + 4, 0);
        m.w32(g + glob::web_hooks + 8 * n, 0);
    }
}

void clear_hooks_unless_given(Mem& m, u32 g, u32 config)
{
    const u32 mime = m.r32(config + 24);
    for (u32 n = 0; n < 11; n++) {
        if (!mime && !m.r32(config + 28) && !m.r32(config + 32) && !m.r32(config + 36) && !m.r32(config + 40)) {
            m.w32(g + glob::web_hooks + 8 * n + 4, 0);
            m.w32(g + glob::web_hooks + 8 * n, 0);
        }
    }
}

// ------------------------------------------------------------------------------------------------- estimates

// cellWebBrowserEstimate2's size: the views (a rect: its pixels at the resolution, 4 bytes each; else per tab 4508877
// bytes, plus 8 MB without the view restriction's bit 0, or 8 MB in all with its bit 3), the heap, 8 MB more with the
// function bit 12, plus 19 MB, rounded down to 1 MB. In libsysutil's single-precision arithmetic.
u32 estimate2_size(Mem& m, u32 g, u32 config)
{
    const u32 extra = ((m.r32(config + 8) >> 12) & 1) << 23;
    s32 views = 0;
    if (m.r8(g + glob::web_rect)) {
        const float res = std::bit_cast<float>(m.r32(config + 60));
        float f = static_cast<float>(static_cast<s32>(m.r32(config + 52)));
        f = f * res;
        f = f * static_cast<float>(static_cast<s32>(m.r32(config + 56)));
        float px = res * f;
        px = px * 4.0f;
        views = fctiwz(px);
    } else {
        const u32 restriction = m.r32(config + 20);
        const float tabs = static_cast<float>(static_cast<s32>(m.r32(config + 12)));
        if (restriction & 8) views = 0x800000;
        else if (restriction & 1) views = fctiwz(tabs * 4508877.0f);
        else views = fctiwz(std::fma(tabs, 4508877.0f, 8388608.0f));
    }
    return (static_cast<u32>(views) + m.r32(config + 4) + extra + 0x12FFFFF) & 0xFFF00000;
}

// cellWebBrowserEstimate's (the first config): per tab 4508877 bytes plus 8 MB, the heap, the function bit 12's 8 MB,
// 19 MB, rounded down to 1 MB.
u32 estimate_size(Mem& m, u32 config)
{
    const float tabs = static_cast<float>(static_cast<s32>(m.r32(config + 20)));
    const s32 views = fctiwz(std::fma(tabs, 4508877.0f, 8388608.0f));
    const u32 extra = ((m.r32(config + 16) >> 12) & 1) << 23;
    return (static_cast<u32>(views) + m.r32(config + 4) + extra + 0x12FFFFF) & 0xFFF00000;
}

// The container's size {total, available} (sys_memory_container_get_size, its answer not looked at: what failed
// leaves the stack's words, here zeros).
void container_size(PPUContext& ctx, Mem& m, u32 container, u32& total, u32& available)
{
    StackVar info(ctx, m.base(), 16);
    lv2_syscall(ctx, kNidMemoryContainerGetSize, {info.addr(), container});
    total = m.r32(info.addr());
    available = m.r32(info.addr() + 4);
}

// Whether the container holds what the browser needs: the estimate in all, and all but 3 MB of it free.
bool memory_fits(PPUContext& ctx, Mem& m, u32 g, u32 config)
{
    const u32 container = m.r32(g + glob::web_container);
    if (container == 0xFFFFFFFF) return false;
    u32 total = 0, available = 0;
    container_size(ctx, m, container, total, available);
    const u32 need = config ? estimate_size(m, config) : 0;
    const u32 free = config ? need - 0x300000 : 0;
    if (total < need) return false;
    return available >= free;
}

bool memory_fits2(PPUContext& ctx, Mem& m, u32 g, u32 config)
{
    const u32 container = m.r32(g + glob::web_container);
    if (container == 0xFFFFFFFF) return false;
    u32 total = 0, available = 0;
    container_size(ctx, m, container, total, available);
    const u32 need = estimate2_size(m, g, config);
    const u32 free = estimate2_size(m, g, config) - 0x300000;
    if (total < need) return false;
    return available >= free;
}

// ------------------------------------------------------------------------------------------------- requests

// A request of libsysutil's web browser code: the request's and the reply's documents on the stack, the packet begun
// (its answer not looked at) on `channel` with ID 0x10E, FixedMemory `channel` taken as the request's allocator, the
// magic "WBRW". finish() ends them in libsysutil's order.
class Request {
public:
    explicit Request(PPUContext& ctx, u32 channel = 0) : ctx_(ctx), channel_(channel), frame_(ctx, guest_base(ctx), 0x100)
    {
        doc = frame_.addr();
        reply = doc + 0x80;
        document_construct(ctx, doc);
        document_construct(ctx, reply);
        packet_begin(ctx, channel, kPacketId);
        memory = fixed_memory_begin(ctx, static_cast<s32>(channel));
        document_use_fixed_memory(ctx, doc, memory);
        document_set_magic(ctx, doc, "WBRW");
    }

    s32 create(std::string_view name, s32 attributes) { return document_create_element(ctx_, doc, name, attributes, elem); }

    // Document::Write into the packet; a failure printed ("send", line 147) and logged once as the handover.
    s32 send(std::string_view what)
    {
        PacketWriter w(ctx_, static_cast<s32>(channel_), kPacketId, doc);
        const s32 r = document_write(ctx_, doc, w);
        if (r != 0) {
            sysutil_error(ctx_, "send", kFile, 147);
            log_handover(ctx_, what, r);
        }
        return r;
    }

    // The reply read into FixedMemory and made a document over it, its element at `root`: the bytes read, or an error
    // (printed as libsysutil prints it: "recv", line 160; a reply that fills the buffer unfinished, line 164).
    s32 receive(Element& root)
    {
        StackVar done(ctx_, guest_base(ctx_), 16);
        const s32 r = packet_read(ctx_, channel_, kPacketId, memory, 0x6000, done.addr());
        if (r < 0) {
            sysutil_error(ctx_, "recv", kFile, 160);
            return r;
        }
        if (!ps_read8(guest_base(ctx_), done.addr()) && r == 0x6000) {
            sysutil_error(ctx_, "recv", kFile, 164);
            return err(PACKET_ERROR_TRUNCATED);
        }
        document_create_from_buffer(ctx_, reply, memory, 0x6000, 0);
        root = document_element(ctx_, reply);
        return r;
    }

    void finish()
    {
        fixed_memory_end(ctx_, static_cast<s32>(channel_));
        packet_end(ctx_, channel_, kPacketId);
        document_clear(ctx_, reply);
        last_clear_ = document_clear(ctx_, doc);
    }
    s32 last_clear() const { return last_clear_; }

    u32 doc = 0, reply = 0, memory = 0;
    Element elem; // the request's element ({0, -1} until made)

private:
    PPUContext& ctx_;
    u32 channel_;
    StackVar frame_;
    s32 last_clear_ = 0;
};

// The reply's answer: its "id" read (a failure printed at `get_line`), a negative ID answered INVALID (printed at
// `neg_line`); else 0.
s32 reply_id(PPUContext& ctx, const Element& root, std::string_view what, s32 get_line, s32 neg_line)
{
    s32 id = 0;
    const s32 r = util_get_int(ctx, root, "id", id);
    if (r != 0) {
        sysutil_error(ctx, what, kFile, get_line);
        return r;
    }
    if (id < 0) {
        sysutil_error(ctx, what, kFile, neg_line);
        return err(CELL_WEBBROWSER_ERROR_INVALID);
    }
    return 0;
}

// A request with a reply whose "id" answers it (Navigate2, WakeupWithGameExit, GetUsrdataOnGameExit): sent, received.
s32 send_and_receive_id(PPUContext& ctx, Request& q, std::string_view what, s32 send_line, s32 recv_line, s32 get_line,
    s32 neg_line)
{
    s32 r = q.send(what);
    if (r != 0) {
        sysutil_error(ctx, what, kFile, send_line);
        return r;
    }
    Element root;
    r = q.receive(root);
    if (r < 0) {
        sysutil_error(ctx, what, kFile, recv_line);
        return r;
    }
    return reply_id(ctx, root, what, get_line, neg_line);
}

// libsysutil's create (FUN_00016954): createFullBrowser with its 21 attributes, the hooks given as flags, a mimeType
// child when the MIME hook is set, the URL; sent, and the reply's "id" its answer.
struct CreateArgs {
    u32 url;
    u8 sync;
    s32 function, restrict, heap, max_tab;
    u8 fullscreen;
    s32 render;
    u8 overflow_auto, transparent;
    s32 x, y, width, height;
    double resolution;
};

s32 create_full(PPUContext& ctx, const CreateArgs& a)
{
    if (!a.url) return err(CELL_WEBBROWSER_ERROR_INVALID);
    constexpr std::string_view what = "create_cmd_createfull";
    Mem m(ctx);
    const u32 g = globals(ctx);
    Request q(ctx);
    q.create("createFullBrowser", 21);
    const auto flag = [&](u32 n) { return hook_cb(m, g, n) != 0 ? 1 : 0; };
    const s32 resolution = fctiwz(static_cast<float>(a.resolution * 100.0));
    s32 r = util_set_int(ctx, q.elem, "heapsize", a.heap);
    r |= util_set_int(ctx, q.elem, "maxtab", a.max_tab);
    r |= util_set_int(ctx, q.elem, "sync", a.sync);
    r |= util_set_int(ctx, q.elem, "function", a.function);
    r |= util_set_int(ctx, q.elem, "fullscreen", a.fullscreen);
    r |= util_set_int(ctx, q.elem, "render", a.render);
    r |= util_set_int(ctx, q.elem, "overflowauto", a.overflow_auto);
    r |= util_set_int(ctx, q.elem, "transparent", a.transparent);
    r |= util_set_int(ctx, q.elem, "posx", a.x);
    r |= util_set_int(ctx, q.elem, "posy", a.y);
    r |= util_set_int(ctx, q.elem, "sizex", a.width);
    r |= util_set_int(ctx, q.elem, "sizey", a.height);
    r |= util_set_int(ctx, q.elem, "resolution", resolution);
    r |= util_set_int(ctx, q.elem, "error", flag(2));
    r |= util_set_int(ctx, q.elem, "status_error", flag(3));
    r |= util_set_int(ctx, q.elem, "notify", flag(4));
    r |= util_set_int(ctx, q.elem, "request", flag(5));
    r |= util_set_int(ctx, q.elem, "restrict", a.restrict);
    r |= util_set_int(ctx, q.elem, "busyicon", m.r8(g + glob::web_busy_icon));
    r |= util_set_string(ctx, q.elem, "hinban", m.r32(g + glob::hinban));
    if (r != 0) {
        sysutil_error(ctx, what, kFile, 524);
        q.finish();
        return r;
    }
    if (hook_cb(m, g, 1) != 0) {
        // A mimeType child, for the unknown MIME type hook.
        Element mime;
        r = document_create_element(ctx, q.doc, "mimeType", 0, mime);
        if (r == 0) r = element_append_child(ctx, q.elem, mime);
        if (r != 0) {
            sysutil_error(ctx, what, kFile, 532);
            q.finish();
            return r;
        }
    }
    r = util_set_string(ctx, q.elem, "url", a.url);
    if (r == 0) {
        r = q.send("cellWebBrowserCreate");
        if (r != 0) {
            sysutil_error(ctx, what, kFile, 542);
        } else {
            Element root;
            r = q.receive(root);
            if (r < 0) {
                sysutil_error(ctx, what, kFile, 551);
            } else {
                s32 id = 0;
                r = util_get_int(ctx, root, "id", id);
                if (r != 0) {
                    sysutil_error(ctx, what, kFile, 559);
                } else if (id < 0) {
                    sysutil_error(ctx, what, kFile, 565);
                    r = id;
                }
            }
        }
    }
    q.finish();
    return r;
}

// A function body: memory that is not there ends it (logged with the call site) with CELL_EFAULT, where libsysutil
// faults.
template <class Body>
s32 guarded(PPUContext& ctx, std::string_view what, Body&& body)
{
    try {
        return body();
    } catch (const Fault& f) {
        log_call_site(ctx, guest_base(ctx),
            std::format("{}: guest memory at {:#010x} is not there (libsysutil faults there): CELL_EFAULT", what,
                f.addr));
        return err(CELL_EFAULT);
    }
}

// ------------------------------------------------------------------------------------------------- the configs

// CellWebBrowserConfig2: version, heap size, functions, tab count, size mode (full screen), view restriction, the hooks
// (unknown MIME type, error, status error, notify, request), the rect {x, y, width, height}, the resolution (a float),
// the magic 0xDEADBEEF. Every function checks the version as libsysutil does (and the pointer, all 64 bits of it).

s32 cellWebBrowserConfig2(PPUContext& ctx, u32 config, u32 version)
{
    if (!config || version != kConfig2Version) return err(CELL_WEBBROWSER_ERROR_INVALID);
    return guarded(ctx, "cellWebBrowserConfig2", [&] {
        Mem m(ctx);
        m.w32(config + 64, kConfig2Magic);
        m.w32(config + 4, 0x3000000);
        m.w32(config + 0, version);
        m.w32(config + 8, 0x28);
        m.w32(config + 12, 4);
        m.w32(config + 24, 0);
        m.w32(config + 16, 0);
        m.w32(config + 20, 0);
        return 0;
    });
}

// The config's version, read where libsysutil reads it: false (INVALID) for no config or another version.
bool config2(Mem& m, u32 config) { return config && m.r32(config) == kConfig2Version; }

s32 cellWebBrowserConfigSetFullVersion2(PPUContext& ctx, u32 config)
{
    return guarded(ctx, "cellWebBrowserConfigSetFullVersion2", [&] {
        Mem m(ctx);
        if (!config2(m, config)) return err(CELL_WEBBROWSER_ERROR_INVALID);
        m.w32(config + 4, 0x3000000);
        m.w32(config + 64, kConfig2Magic);
        m.w32(config + 8, 0x28);
        m.w32(config + 12, 4);
        m.w32(config + 20, 0);
        m.w32(config + 16, 0);
        m.w32(config + 24, 0);
        return 0;
    });
}

// The hooks: the callback into the config and libsysutil's table, with its userdata.
s32 set_hook(PPUContext& ctx, std::string_view what, u32 config, u32 n, u32 cb, u32 userdata)
{
    return guarded(ctx, what, [&] {
        Mem m(ctx);
        if (!config2(m, config)) return err(CELL_WEBBROWSER_ERROR_INVALID);
        const u32 hook = globals(ctx) + glob::web_hooks + 8 * n;
        if (n == 5) {
            m.w32(config + 40, cb);
            m.w32(hook + 4, userdata);
        } else {
            m.w32(hook + 4, userdata);
            m.w32(config + 20 + 4 * n, cb);
        }
        m.w32(hook, cb);
        return 0;
    });
}

s32 cellWebBrowserConfigSetUnknownMIMETypeHook2(PPUContext& ctx, u32 config, u32 cb, u32 userdata)
{
    return set_hook(ctx, "cellWebBrowserConfigSetUnknownMIMETypeHook2", config, 1, cb, userdata);
}
s32 cellWebBrowserConfigSetErrorHook2(PPUContext& ctx, u32 config, u32 cb, u32 userdata)
{
    return set_hook(ctx, "cellWebBrowserConfigSetErrorHook2", config, 2, cb, userdata);
}
s32 cellWebBrowserConfigSetStatusHook2(PPUContext& ctx, u32 config, u32 cb, u32 userdata)
{
    return set_hook(ctx, "cellWebBrowserConfigSetStatusHook2", config, 3, cb, userdata);
}
s32 cellWebBrowserConfigSetNotifyHook2(PPUContext& ctx, u32 config, u32 cb, u32 userdata)
{
    return set_hook(ctx, "cellWebBrowserConfigSetNotifyHook2", config, 4, cb, userdata);
}
s32 cellWebBrowserConfigSetRequestHook2(PPUContext& ctx, u32 config, u32 cb, u32 userdata)
{
    return set_hook(ctx, "cellWebBrowserConfigSetRequestHook2", config, 5, cb, userdata);
}

// A word of the config, after the version check (and `valid`'s on the value).
template <class Valid>
s32 set_word(PPUContext& ctx, std::string_view what, u32 config, u32 at, u32 value, Valid&& valid)
{
    return guarded(ctx, what, [&] {
        Mem m(ctx);
        if (!config2(m, config) || !valid()) return err(CELL_WEBBROWSER_ERROR_INVALID);
        m.w32(config + at, value);
        return 0;
    });
}

s32 cellWebBrowserConfigSetFunction2(PPUContext& ctx, u32 config, u32 functions)
{
    return set_word(ctx, "cellWebBrowserConfigSetFunction2", config, 8, functions, [] { return true; });
}
s32 cellWebBrowserConfigSetViewCondition2(PPUContext& ctx, u32 config, u32 condition)
{
    return set_word(ctx, "cellWebBrowserConfigSetViewCondition2", config, 20, condition, [] { return true; });
}
// A heap of at least 2 MB.
s32 cellWebBrowserConfigSetHeapSize2(PPUContext& ctx, u32 config, s32 size)
{
    return set_word(ctx, "cellWebBrowserConfigSetHeapSize2", config, 4, static_cast<u32>(size),
        [&] { return size >= 0x200000; });
}
// One to six tabs.
s32 cellWebBrowserConfigSetTabCount2(PPUContext& ctx, u32 config, u32 tabs)
{
    return set_word(ctx, "cellWebBrowserConfigSetTabCount2", config, 12, tabs, [&] { return tabs - 1 <= 5; });
}
s32 cellWebBrowserConfigSetFullScreen2(PPUContext& ctx, u32 config, u32 full)
{
    return set_word(ctx, "cellWebBrowserConfigSetFullScreen2", config, 16, full, [&] { return full <= 1; });
}

s32 cellWebBrowserConfigGetHeapSize2(PPUContext& ctx, u32 config, u32 size)
{
    return guarded(ctx, "cellWebBrowserConfigGetHeapSize2", [&] {
        Mem m(ctx);
        if (!config2(m, config) || !size) return err(CELL_WEBBROWSER_ERROR_INVALID);
        m.w32(size, m.r32(config + 4));
        return 0;
    });
}

// cellWebBrowserConfigSetViewRect2(config, CellWebBrowserRect rect, float resolution): the rect comes in r4 and r5
// (x and y, width and height), the resolution in f1. libsysutil keeps whether the rect has an area (it makes the
// estimate count the rect's pixels).
s32 cellWebBrowserConfigSetViewRect2(PPUContext& ctx, u32 config, u64 position, u64 size, float resolution)
{
    return guarded(ctx, "cellWebBrowserConfigSetViewRect2", [&] {
        Mem m(ctx);
        if (!config2(m, config)) return err(CELL_WEBBROWSER_ERROR_INVALID);
        const s32 width = static_cast<s32>(size >> 32), height = static_cast<s32>(size);
        m.w32(config + 60, std::bit_cast<u32>(resolution));
        m.w32(config + 44, static_cast<u32>(position >> 32));
        m.w32(config + 48, static_cast<u32>(position));
        m.w32(config + 52, static_cast<u32>(width));
        m.w32(config + 56, static_cast<u32>(height));
        m.w8(globals(ctx) + glob::web_rect, width > 0 && height > 0 ? 1 : 0);
        return 0;
    });
}

s32 cellWebBrowserEstimate2(PPUContext& ctx, u32 config, u32 mem_size)
{
    return guarded(ctx, "cellWebBrowserEstimate2", [&] {
        Mem m(ctx);
        if (!config2(m, config) || !mem_size) return err(CELL_WEBBROWSER_ERROR_INVALID);
        m.w32(mem_size, estimate2_size(m, globals(ctx), config));
        return 0;
    });
}

// The first config (CellWebBrowserConfig): version, heap size, MIME sets and their count, functions, tab count, the
// exit, download and navigated callbacks. These functions check nothing but the pointer; r3 is left as libsysutil
// leaves it (the config).

u32 cellWebBrowserConfig(PPUContext& ctx, u32 config)
{
    if (!config) return 0;
    guarded(ctx, "cellWebBrowserConfig", [&] {
        Mem m(ctx);
        m.w32(config + 0, 0x10000);
        m.w32(config + 4, 0x3000000);
        m.w32(config + 32, 0);
        m.w32(config + 16, 0x38);
        m.w32(config + 20, 4);
        m.w32(config + 8, 0);
        m.w32(config + 24, 0);
        m.w32(config + 28, 0);
        return 0;
    });
    return config;
}

u32 cellWebBrowserConfigWithVer(PPUContext& ctx, u32 config, u32 version)
{
    if (!config) return 0;
    guarded(ctx, "cellWebBrowserConfigWithVer", [&] {
        Mem m(ctx);
        m.w32(config + 4, 0x3000000);
        m.w32(config + 32, 0);
        m.w32(config + 0, version);
        m.w32(config + 16, 0x38);
        m.w32(config + 20, 4);
        m.w32(config + 8, 0);
        m.w32(config + 24, 0);
        m.w32(config + 28, 0);
        return 0;
    });
    return config;
}

u32 cellWebBrowserConfigSetVersion(PPUContext& ctx, u32 config, u32 version)
{
    if (!config) return 0;
    guarded(ctx, "cellWebBrowserConfigSetVersion", [&] {
        Mem m(ctx);
        m.w32(config, version);
        return 0;
    });
    return config;
}

s32 cellWebBrowserConfigSetMimeSet(PPUContext&) { return 0; } // libsysutil keeps no MIME sets

s32 set_v1_word(PPUContext& ctx, std::string_view what, u32 config, u32 at, u32 value)
{
    if (!config) return 0;
    return guarded(ctx, what, [&] {
        Mem m(ctx);
        m.w32(config + at, value);
        return 0;
    });
}

s32 cellWebBrowserConfigSetFunction(PPUContext& ctx, u32 config, u32 functions)
{
    return set_v1_word(ctx, "cellWebBrowserConfigSetFunction", config, 16, functions);
}
s32 cellWebBrowserConfigSetCustomExit(PPUContext& ctx, u32 config, u32 exit)
{
    return set_v1_word(ctx, "cellWebBrowserConfigSetCustomExit", config, 24, exit);
}
s32 cellWebBrowserConfigSetHeapSize(PPUContext& ctx, u32 config, u32 size)
{
    return set_v1_word(ctx, "cellWebBrowserConfigSetHeapSize", config, 4, size);
}
// The tab count; one for anything below two.
s32 cellWebBrowserConfigSetDisableTabs(PPUContext& ctx, u32 config, s32 tabs)
{
    return set_v1_word(ctx, "cellWebBrowserConfigSetDisableTabs", config, 20, tabs < 2 ? 1u : static_cast<u32>(tabs));
}

s32 cellWebBrowserConfigGetHeapSize(PPUContext& ctx, u32 config)
{
    if (!config) return 0;
    return guarded(ctx, "cellWebBrowserConfigGetHeapSize", [&] {
        Mem m(ctx);
        return static_cast<s32>(m.r32(config + 4));
    });
}

u32 cellWebBrowserEstimate(PPUContext& ctx, u32 config)
{
    if (!config) return 0;
    return static_cast<u32>(guarded(ctx, "cellWebBrowserEstimate", [&] {
        Mem m(ctx);
        return static_cast<s32>(estimate_size(m, config));
    }));
}

s32 cellWebBrowserSetSystemCallbackUsrdata(PPUContext& ctx, u32 userdata)
{
    return guarded(ctx, "cellWebBrowserSetSystemCallbackUsrdata", [&] {
        Mem m(ctx);
        m.w32(globals(ctx) + glob::web_system_ud, userdata);
        return 0;
    });
}

// The title ID of local contents a created browser is told (any non-null pointer; libsysutil keeps it).
s32 cellWebBrowserSetLocalContentsAdditionalTitleID(PPUContext& ctx, u64 title_id)
{
    if (!title_id) return err(CELL_WEBBROWSER_ERROR_INVALID);
    return guarded(ctx, "cellWebBrowserSetLocalContentsAdditionalTitleID", [&] {
        Mem m(ctx);
        m.w32(globals(ctx) + glob::hinban, static_cast<u32>(title_id));
        return 0;
    });
}

// The web component: libsysutil has none (its functions answer 0; Destroy leaves r3 as it came).
s32 cellWebComponentCreate(PPUContext&) { return 0; }
s32 cellWebComponentCreateAsync(PPUContext&) { return 0; }
u32 cellWebComponentDestroy(PPUContext&, u32 r3) { return r3; }

// ------------------------------------------------------------------------------------------------- the browser

// cellWebBrowserInitialize(system callback, memory container): the container kept (not -1), the callback kept with
// userdata 0, libsysutil's dispatcher for the system's browser events registered (a failure printed, line 423, and the
// request not sent), then "initialize" with the container ("mc") sent.
s32 cellWebBrowserInitialize(PPUContext& ctx, u32 system_cb, s32 container)
{
    packets_start(ctx);
    constexpr std::string_view what = "cellWebBrowserInitialize";
    return guarded(ctx, what, [&] {
        Mem m(ctx);
        const u32 g = globals(ctx);
        if (container != -1) m.w32(g + glob::web_container, static_cast<u32>(container));
        m.w32(g + glob::web_system_cb, system_cb);
        m.w32(g + glob::web_system_ud, 0);
        const s32 registered = register_dispatcher(ctx, kDispatcherEvent, event_handler());
        if (registered != 0) sysutil_error(ctx, what, kFile, 423);
        Request q(ctx);
        q.create("initialize", 1);
        s32 r = registered | util_set_int(ctx, q.elem, "mc", static_cast<s32>(m.r32(g + glob::web_container)));
        if (r != 0) {
            sysutil_error(ctx, what, kFile, 431);
        } else if ((r = q.send(what)) != 0) {
            sysutil_error(ctx, what, kFile, 436);
        }
        q.finish();
        return r;
    });
}

// cellWebBrowserShutdown(): "finalize" sent (the system's "shut down" event would unregister the dispatcher). r3 is
// the send's answer.
s32 cellWebBrowserShutdown(PPUContext& ctx)
{
    packets_start(ctx);
    constexpr std::string_view what = "cellWebBrowserShutdown";
    return guarded(ctx, what, [&] {
        Request q(ctx);
        q.create("finalize", 0);
        const s32 r = q.send(what);
        if (r != 0) sysutil_error(ctx, what, kFile, 472);
        q.finish();
        return r;
    });
}

// cellWebBrowserDestroy(): "destroyFullBrowser" sent when a browser was made (the flag stays: the system's "unloaded"
// event clears it). r3 is left as libsysutil leaves it: the request document's last Clear.
s32 cellWebBrowserDestroy(PPUContext& ctx)
{
    packets_start(ctx);
    constexpr std::string_view what = "cellWebBrowserDestroy";
    return guarded(ctx, what, [&] {
        Mem m(ctx);
        if (!m.r8(globals(ctx) + glob::web_created)) return 0;
        Request q(ctx);
        q.create("destroyFullBrowser", 0);
        if (q.send(what) != 0) sysutil_error(ctx, what, kFile, 1522);
        q.finish();
        return q.last_clear();
    });
}

s32 cellWebBrowserDestroy2(PPUContext& ctx)
{
    cellWebBrowserDestroy(ctx);
    return 0;
}

// "activate" / "deactivate" (with the view's alpha, a float in f1): sent and the reply read; its byte count answers.
s32 activate(PPUContext& ctx, std::string_view what, std::string_view element, const float* alpha)
{
    packets_start(ctx);
    return guarded(ctx, what, [&] {
        Request q(ctx);
        q.create(element, alpha ? 1 : 0);
        if (alpha) util_set_float(ctx, q.elem, "alpha", *alpha);
        s32 r = q.send(what);
        if (r == 0) {
            Element root;
            r = q.receive(root);
        }
        q.finish();
        return r;
    });
}

s32 cellWebBrowserActivate(PPUContext& ctx) { return activate(ctx, "cellWebBrowserActivate", "activate", nullptr); }
s32 cellWebBrowserDeactivate(PPUContext& ctx, float alpha)
{
    return activate(ctx, "cellWebBrowserDeactivate", "deactivate", &alpha);
}

// cellWebBrowserGetUsrdataOnGameExit(callback): the callback kept (hook 10), the dispatcher registered (a failure
// printed, line 1214), then "GetUsrdataOnGameExit" sent and its reply's "id" checked.
s32 cellWebBrowserGetUsrdataOnGameExit(PPUContext& ctx, u64 callback)
{
    if (!callback) return err(CELL_WEBBROWSER_ERROR_INVALID);
    packets_start(ctx);
    constexpr std::string_view what = "cellWebBrowserGetUsrdataOnGameExit";
    return guarded(ctx, what, [&] {
        Mem m(ctx);
        m.w32(globals(ctx) + glob::web_hooks + 8 * 10, static_cast<u32>(callback));
        if (register_dispatcher(ctx, kDispatcherEvent, event_handler()) != 0) sysutil_error(ctx, what, kFile, 1214);
        Request q(ctx);
        q.create("GetUsrdataOnGameExit", 0);
        const s32 r = send_and_receive_id(ctx, q, what, 1224, 1233, 1241, 1247);
        q.finish();
        return r;
    });
}

// cellWebBrowserWakeupWithGameExit(url, usrdata): "wakeupWithGameExit" with them sent, its reply's "id" checked.
s32 cellWebBrowserWakeupWithGameExit(PPUContext& ctx, u32 url, s32 usrdata)
{
    if (!url) return err(CELL_WEBBROWSER_ERROR_INVALID);
    packets_start(ctx);
    constexpr std::string_view what = "cellWebBrowserWakeupWithGameExit";
    return guarded(ctx, what, [&] {
        Request q(ctx);
        q.create("wakeupWithGameExit", 2);
        s32 r = util_set_int(ctx, q.elem, "usrdata", usrdata);
        if (r != 0) sysutil_error(ctx, what, kFile, 1159);
        else if ((r = util_set_string(ctx, q.elem, "url", url)) == 0)
            r = send_and_receive_id(ctx, q, what, 1167, 1176, 1184, 1190);
        q.finish();
        return r;
    });
}

// cellWebBrowserNavigate2(url): "navigateBrowser" sent, its reply's "id" checked.
s32 cellWebBrowserNavigate2(PPUContext& ctx, u32 url)
{
    if (!url) return err(CELL_WEBBROWSER_ERROR_INVALID);
    packets_start(ctx);
    constexpr std::string_view what = "cellWebBrowserNavigate2";
    return guarded(ctx, what, [&] {
        Request q(ctx);
        q.create("navigateBrowser", 1);
        s32 r = util_set_string(ctx, q.elem, "url", url);
        if (r == 0) r = send_and_receive_id(ctx, q, what, 971, 980, 988, 994);
        q.finish();
        return r;
    });
}

// cellWebBrowserUpdatePointerDisplayPos2(CellWebBrowserPos pos): x and y in r3, "pointer" sent with them and mode 0.
s32 cellWebBrowserUpdatePointerDisplayPos2(PPUContext& ctx, u64 position)
{
    packets_start(ctx);
    constexpr std::string_view what = "cellWebBrowserUpdatePointerDisplayPos2";
    return guarded(ctx, what, [&] {
        Request q(ctx);
        q.create("pointer", 3);
        s32 r = util_set_int(ctx, q.elem, "pointerX", static_cast<s32>(position >> 32));
        r |= util_set_int(ctx, q.elem, "pointerY", static_cast<s32>(position));
        r |= util_set_int(ctx, q.elem, "pointerMode", 0);
        if (r != 0) sysutil_error(ctx, what, kFile, 1129);
        else if ((r = q.send(what)) != 0) sysutil_error(ctx, what, kFile, 1133);
        q.finish();
        return r;
    });
}

// The creates. Each refuses a second browser (BUSY), checks its arguments (INVALID) and the container's memory
// (NOMEM), marks the browser made, then sends createFullBrowser.

// cellWebBrowserCreate(url): the first config's defaults, every hook cleared first.
s32 cellWebBrowserCreate(PPUContext& ctx, u32 url)
{
    packets_start(ctx);
    return guarded(ctx, "cellWebBrowserCreate", [&] {
        Mem m(ctx);
        const u32 g = globals(ctx);
        clear_hooks(m, g);
        if (m.r8(g + glob::web_created)) return err(CELL_WEBBROWSER_ERROR_BUSY);
        if (!url) return err(CELL_WEBBROWSER_ERROR_INVALID);
        StackVar config(ctx, m.base(), 0x40);
        cellWebBrowserConfigWithVer(ctx, config.addr(), 0x10000);
        if (!memory_fits(ctx, m, g, config.addr())) return err(CELL_WEBBROWSER_ERROR_NOMEM);
        m.w8(g + glob::web_created, 1);
        return create_full(ctx, {url, 1, 0xFFFF, 0, 0x3000000, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0.0});
    });
}

// cellWebBrowserCreateWithConfig(Full)(url, config): the first config's heap, functions and tabs, full screen or not.
s32 create_with_config(PPUContext& ctx, std::string_view what, u32 url, u32 config, u8 fullscreen)
{
    packets_start(ctx);
    return guarded(ctx, what, [&] {
        Mem m(ctx);
        const u32 g = globals(ctx);
        if (m.r8(g + glob::web_created)) return err(CELL_WEBBROWSER_ERROR_BUSY);
        if (!config || !url) return err(CELL_WEBBROWSER_ERROR_INVALID);
        if (!memory_fits(ctx, m, g, config)) return err(CELL_WEBBROWSER_ERROR_NOMEM);
        m.w8(g + glob::web_created, 1);
        return create_full(ctx, {url, 1, static_cast<s32>(m.r32(config + 16)), 0, static_cast<s32>(m.r32(config + 4)),
                                    static_cast<s32>(m.r32(config + 20)), fullscreen, 0, 0, 0, 0, 0, 0, 0, 0.0});
    });
}

s32 cellWebBrowserCreateWithConfig(PPUContext& ctx, u32 url, u32 config)
{
    return create_with_config(ctx, "cellWebBrowserCreateWithConfig", url, config, 0);
}
s32 cellWebBrowserCreateWithConfigFull(PPUContext& ctx, u32 url, u32 config)
{
    return create_with_config(ctx, "cellWebBrowserCreateWithConfigFull", url, config, 1);
}

// The second config's checks: the URL, the config, its version and magic.
s32 check_config2(Mem& m, u32 config, u32 url)
{
    if (!url || !config || m.r32(config) != kConfig2Version || m.r32(config + 64) != kConfig2Magic)
        return err(CELL_WEBBROWSER_ERROR_INVALID);
    return 0;
}

// cellWebBrowserCreateRenderWithRect2 / CreateWithRect2 (config, url, overflow auto, transparent, CellWebBrowserRect
// in r7 and r8, resolution in f1): a rendered view (render 1) or a window (render 2) at the rect, one tab.
s32 create_with_rect(PPUContext& ctx, std::string_view what, u32 config, u32 url, u64 overflow_auto, u64 transparent,
    u64 position, u64 size, double resolution, s32 render)
{
    packets_start(ctx);
    return guarded(ctx, what, [&] {
        Mem m(ctx);
        if (!url) return err(CELL_WEBBROWSER_ERROR_INVALID);
        if (const s32 r = check_config2(m, config, url)) return r;
        const u32 g = globals(ctx);
        if (m.r8(g + glob::web_created)) return err(CELL_WEBBROWSER_ERROR_BUSY);
        if (!memory_fits2(ctx, m, g, config)) return err(CELL_WEBBROWSER_ERROR_NOMEM);
        clear_hooks_unless_given(m, g, config);
        m.w8(g + glob::web_created, 1);
        return create_full(ctx,
            {url, 1, static_cast<s32>(m.r32(config + 8)), static_cast<s32>(m.r32(config + 20)),
                static_cast<s32>(m.r32(config + 4)), 1, 1, render, static_cast<u8>(overflow_auto),
                static_cast<u8>(transparent), static_cast<s32>(position >> 32), static_cast<s32>(position),
                static_cast<s32>(size >> 32), static_cast<s32>(size), resolution});
    });
}

s32 cellWebBrowserCreateRenderWithRect2(PPUContext& ctx, u32 config, u32 url, u64 overflow_auto, u64 transparent,
    u64 position, u64 size, double resolution)
{
    return create_with_rect(ctx, "cellWebBrowserCreateRenderWithRect2", config, url, overflow_auto, transparent,
        position, size, resolution, 1);
}

s32 cellWebBrowserCreateWithRect2(PPUContext& ctx, u32 config, u32 url, u64 overflow_auto, u64 transparent,
    u64 position, u64 size, double resolution)
{
    return create_with_rect(ctx, "cellWebBrowserCreateWithRect2", config, url, overflow_auto, transparent, position,
        size, resolution, 2);
}

// cellWebBrowserCreateRender2(config, url, overflow auto, transparent): CreateRenderWithRect2 with no rect at 0.0.
s32 cellWebBrowserCreateRender2(PPUContext& ctx, u32 config, u32 url, u64 overflow_auto, u64 transparent)
{
    return create_with_rect(ctx, "cellWebBrowserCreateRender2", config, url, overflow_auto, transparent, 0, 0, 0.0, 1);
}

// cellWebBrowserCreate2(config, url): the second config's settings; its view condition's bits pick the busy icon (16),
// a window (8, or a rect with an area: render 2), overflow auto (4) and transparency (2); its rect and resolution when
// one was set.
s32 cellWebBrowserCreate2(PPUContext& ctx, u32 config, u32 url)
{
    packets_start(ctx);
    return guarded(ctx, "cellWebBrowserCreate2", [&] {
        Mem m(ctx);
        if (const s32 r = check_config2(m, config, url)) return r;
        const u32 g = globals(ctx);
        if (m.r8(g + glob::web_created)) return err(CELL_WEBBROWSER_ERROR_BUSY);
        if (!memory_fits2(ctx, m, g, config)) return err(CELL_WEBBROWSER_ERROR_NOMEM);
        clear_hooks_unless_given(m, g, config);
        const u32 condition = m.r32(config + 20);
        m.w8(g + glob::web_created, 1);
        if (condition & 0x10) m.w8(g + glob::web_busy_icon, 1);
        s32 render = static_cast<s32>((condition >> 3) & 1);
        double resolution = 0.0;
        s32 x = 0, y = 0, width = 0, height = 0;
        if (m.r8(g + glob::web_rect)) {
            x = static_cast<s32>(m.r32(config + 44));
            y = static_cast<s32>(m.r32(config + 48));
            width = static_cast<s32>(m.r32(config + 52));
            height = static_cast<s32>(m.r32(config + 56));
            resolution = static_cast<double>(std::bit_cast<float>(m.r32(config + 60)));
            if (render == 0) render = 2;
        }
        return create_full(ctx,
            {url, 1, static_cast<s32>(m.r32(config + 8)), static_cast<s32>(condition), static_cast<s32>(m.r32(config + 4)),
                static_cast<s32>(m.r32(config + 12)), static_cast<u8>(m.r32(config + 16) != 0 ? 1 : 0), render,
                static_cast<u8>((condition >> 2) & 1), static_cast<u8>((condition >> 1) & 1), x, y, width, height,
                resolution});
    });
}

// ------------------------------------------------------------------------------------------------- system events

// The data of a hook's call, read from the system on channel 1 into a document over FixedMemory 1 (the reply document
// is the one given the allocator and the magic here); a read that fails is printed ("recv" 160 or 164, then the
// entry's line) and nothing is called.
class EventData {
public:
    explicit EventData(PPUContext& ctx) : ctx_(ctx), frame_(ctx, guest_base(ctx), 0x100)
    {
        doc_ = frame_.addr();
        reply = doc_ + 0x80;
        document_construct(ctx, doc_);
        document_construct(ctx, reply);
        packet_begin(ctx, 1, kPacketId);
        memory_ = fixed_memory_begin(ctx, 1);
        document_use_fixed_memory(ctx, reply, memory_);
        document_set_magic(ctx, reply, "WBRW");
    }

    bool read(std::string_view entry, s32 line)
    {
        StackVar done(ctx_, guest_base(ctx_), 16);
        const s32 r = packet_read(ctx_, 1, kPacketId, memory_, 0x6000, done.addr());
        if (r < 0 || (!ps_read8(guest_base(ctx_), done.addr()) && r == 0x6000)) {
            sysutil_error(ctx_, "recv", kFile, r < 0 ? 160 : 164);
            sysutil_error(ctx_, entry, kFile, line);
            return false;
        }
        document_create_from_buffer(ctx_, reply, memory_, 0x6000, 0);
        root = document_element(ctx_, reply);
        return true;
    }

    void finish()
    {
        fixed_memory_end(ctx_, 1);
        packet_end(ctx_, 1, kPacketId);
        document_clear(ctx_, reply);
        document_clear(ctx_, doc_);
    }

    u32 reply = 0;
    Element root;

private:
    PPUContext& ctx_;
    StackVar frame_;
    u32 doc_ = 0, memory_ = 0;
};

// A string attribute of the event's data, as libsysutil reads it (its address left 0 when it isn't there).
s32 event_string(PPUContext& ctx, const Element& root, std::string_view name, u32& out)
{
    StackVar s(ctx, guest_base(ctx), 16);
    const Element e = root;
    const s32 r = util_get_string(ctx, e, name, s.addr(), s.addr() + 4);
    out = ps_read32(guest_base(ctx), s.addr());
    return r;
}

// libsysutil's dispatcher for the system's web browser events (event 0x112: the status in the low word of the
// first argument, the kind in its high word, the second argument the event's own). A status (kind 0) goes to the system
// callback (with the userdata set, else the event's); "unloaded" (7) clears the made flag, "shut down" (4) unregisters
// the dispatcher and clears the hooks and the callback. A hook's call reads its data from the system on channel 1 (1:
// the unknown MIME type's type and URL, 2: an error, 3: a status error, 4: a notification's text, 5: a request, 10: the
// usrdata on game exit, after which the hook and the dispatcher go); other kinds call the hook of that number. The
// system software sends these; pspocket's console sends none, so nothing calls this but a test (it is here so the
// dispatcher libsysutil registers is libsysutil's).
s32 cellSysutil_WebBrowserSystemEvent(PPUContext& ctx, u64 event, u32 param)
{
    return guarded(ctx, kEventHandler, [&] {
        Mem m(ctx);
        const u32 g = globals(ctx);
        const u32 kind = static_cast<u32>(event >> 32);
        if (kind == 0) {
            if (event == 7) m.w8(g + glob::web_created, 0);
            if (const u32 cb = m.r32(g + glob::web_system_cb)) {
                const u32 ud = m.r32(g + glob::web_system_ud);
                call_guest(ctx, cb, {static_cast<u64>(static_cast<s64>(static_cast<s32>(event))), ud ? ud : param});
            }
            if (static_cast<u32>(event) == 4) {
                sysutil_call(ctx, kNidUnregisterDispatcher, {kDispatcherEvent});
                clear_hooks(m, g);
                m.w32(g + glob::web_system_cb, 0);
            }
            return 0;
        }
        const auto hook = [&](u32 n, u32& ud) {
            ud = m.r32(g + glob::web_hooks + 8 * n + 4);
            return m.r32(g + glob::web_hooks + 8 * n);
        };
        u32 ud = 0;
        switch (kind) {
        case 1: { // the unknown MIME type: (type, URL, userdata)
            const u32 cb = hook(1, ud);
            if (!cb) return 0;
            EventData e(ctx);
            if (e.read("call_cb_entry2", 238)) {
                u32 type = 0, url = 0;
                s32 r = event_string(ctx, e.root, "mimeType", type);
                r |= event_string(ctx, e.root, "url", url);
                if (r != 0) sysutil_error(ctx, "call_cb_entry2", kFile, 246);
                call_guest(ctx, cb, {type, url, ud});
            }
            e.finish();
            return 0;
        }
        case 4: { // a notification: (text, userdata)
            const u32 cb = hook(4, ud);
            if (!cb) return 0;
            EventData e(ctx);
            if (e.read("call_cb_entry3", 268)) {
                u32 text = 0;
                if (event_string(ctx, e.root, "notify", text) != 0) sysutil_error(ctx, "call_cb_entry3", kFile, 275);
                call_guest(ctx, cb, {text, ud});
            }
            e.finish();
            return 0;
        }
        case 2:   // an error: (error, userdata)
        case 3:   // a status error: (error, userdata)
        case 5: { // a request: (request, the data's document, userdata)
            const u32 cb = hook(kind, ud);
            if (!cb) return 0;
            const std::string_view entry = kind == 2 ? "call_cb_entry4" : kind == 3 ? "call_cb_entry5" : "call_cb_entry6";
            const std::string_view name = kind == 2 ? "error" : kind == 3 ? "status_error" : "request";
            const s32 read_line = kind == 2 ? 294 : kind == 3 ? 320 : 347;
            const s32 get_line = kind == 2 ? 301 : kind == 3 ? 327 : 354;
            EventData e(ctx);
            if (e.read(entry, read_line)) {
                s32 v = 0;
                if (util_get_int(ctx, e.root, name, v) != 0) sysutil_error(ctx, entry, kFile, get_line);
                if (kind == 5) call_guest(ctx, cb, {static_cast<u64>(static_cast<s64>(v)), e.reply, ud});
                else call_guest(ctx, cb, {static_cast<u64>(static_cast<s64>(v)), ud});
            }
            e.finish();
            return 0;
        }
        case 10: { // the usrdata on game exit: (usrdata); then the hook and the dispatcher go
            const u32 hook_at = g + glob::web_hooks + 8 * 10;
            if (const u32 cb = m.r32(hook_at)) {
                EventData e(ctx);
                if (e.read("call_cb_entry7", 372)) {
                    s32 v = 0;
                    if (util_get_int(ctx, e.root, "usrdata", v) != 0) sysutil_error(ctx, "call_cb_entry7", kFile, 379);
                    call_guest(ctx, cb, {static_cast<u32>(v)});
                }
                e.finish();
            }
            m.w32(hook_at, 0);
            sysutil_call(ctx, kNidUnregisterDispatcher, {kDispatcherEvent});
            return 0;
        }
        default: { // another hook: (kind, 0, userdata)
            const u32 cb = hook(kind, ud);
            if (cb) call_guest(ctx, cb, {static_cast<u64>(static_cast<s64>(static_cast<s32>(kind))), 0, ud});
            return 0;
        }
        }
    });
}

} // namespace

void register_web_browser(Registry& r)
{
    r.add("cellSysutil", {
        PS_HLE_FUNC(0x7FB3C6A5, cellWebBrowserConfig2),
        PS_HLE_FUNC(0x52D9457A, cellWebBrowserConfigSetFullVersion2),
        PS_HLE_FUNC(0xA9A439E0, cellWebBrowserConfigSetUnknownMIMETypeHook2),
        PS_HLE_FUNC(0x1F6629E4, cellWebBrowserConfigSetErrorHook2),
        PS_HLE_FUNC(0xAC16777E, cellWebBrowserConfigSetStatusHook2),
        PS_HLE_FUNC(0x93C523C6, cellWebBrowserConfigSetNotifyHook2),
        PS_HLE_FUNC(0xEE7528F3, cellWebBrowserConfigSetRequestHook2),
        PS_HLE_FUNC(0xE22ED55D, cellWebBrowserConfigSetFunction2),
        PS_HLE_FUNC(0xD7A617F5, cellWebBrowserConfigSetViewCondition2),
        PS_HLE_FUNC(0xA9B62AC8, cellWebBrowserConfigSetViewRect2),
        PS_HLE_FUNC(0xFE669845, cellWebBrowserConfigSetHeapSize2),
        PS_HLE_FUNC(0x0C316B87, cellWebBrowserConfigGetHeapSize2),
        PS_HLE_FUNC(0x619B1427, cellWebBrowserConfigSetTabCount2),
        PS_HLE_FUNC(0x1A91874B, cellWebBrowserConfigSetFullScreen2),
        PS_HLE_FUNC(0x6D087930, cellWebBrowserEstimate2),
        PS_HLE_FUNC(0x83668B8E, cellWebBrowserConfig),
        PS_HLE_FUNC(0x6C1082AA, cellWebBrowserConfigWithVer),
        PS_HLE_FUNC(0x60A4DAAB, cellWebBrowserConfigSetVersion),
        PS_HLE_FUNC(0x0F8A3B6B, cellWebBrowserConfigSetMimeSet),
        PS_HLE_FUNC(0x9C9FE6A4, cellWebBrowserConfigSetFunction),
        PS_HLE_FUNC(0x3261A9C8, cellWebBrowserConfigSetDisableTabs),
        PS_HLE_FUNC(0x7F896A1F, cellWebBrowserConfigSetCustomExit),
        PS_HLE_FUNC(0x76FC8FB1, cellWebBrowserConfigSetHeapSize),
        PS_HLE_FUNC(0xA4DD11CC, cellWebBrowserConfigGetHeapSize),
        PS_HLE_FUNC(0x58C89C4A, cellWebBrowserEstimate),
        PS_HLE_FUNC(0x6DFFF31D, cellWebBrowserSetSystemCallbackUsrdata),
        PS_HLE_FUNC(0xC8971DB5, cellWebBrowserSetLocalContentsAdditionalTitleID),
        PS_HLE_FUNC(0x8A5DBB58, cellWebComponentCreate),
        PS_HLE_FUNC(0x6144F033, cellWebComponentCreateAsync),
        PS_HLE_FUNC(0xDCE51399, cellWebComponentDestroy),
        PS_HLE_FUNC(0x749C9B5F, cellWebBrowserInitialize),
        PS_HLE_FUNC(0x93CED48D, cellWebBrowserShutdown),
        PS_HLE_FUNC(0xBED85CB8, cellWebBrowserDestroy),
        PS_HLE_FUNC(0xE8DEE79C, cellWebBrowserDestroy2),
        PS_HLE_FUNC(0x6D7444E6, cellWebBrowserActivate),
        PS_HLE_FUNC(0x46A0285D, cellWebBrowserDeactivate),
        PS_HLE_FUNC(0x10CABEFF, cellWebBrowserGetUsrdataOnGameExit),
        PS_HLE_FUNC(0x24644561, cellWebBrowserWakeupWithGameExit),
        PS_HLE_FUNC(0x9FB97B10, cellWebBrowserNavigate2),
        PS_HLE_FUNC(0x425FEF23, cellWebBrowserUpdatePointerDisplayPos2),
        PS_HLE_FUNC(0x7F881BE1, cellWebBrowserCreate),
        PS_HLE_FUNC(0x8A4CB646, cellWebBrowserCreateWithConfig),
        PS_HLE_FUNC(0xB53B3D42, cellWebBrowserCreateWithConfigFull),
        PS_HLE_FUNC(0x4B6E8560, cellWebBrowserCreateRenderWithRect2),
        PS_HLE_FUNC(0xA58943F8, cellWebBrowserCreateWithRect2),
        PS_HLE_FUNC(0xF1A443E7, cellWebBrowserCreateRender2),
        PS_HLE_FUNC(0xA5F12145, cellWebBrowserCreate2),
        PS_HLE_INTERNAL(recomp::nid_of(kEventHandler), cellSysutil_WebBrowserSystemEvent),
    });
}

} // namespace pspocket::hle::sysutil
