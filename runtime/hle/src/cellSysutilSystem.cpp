// cellSysutil's other requests to the system software, the output callbacks and the save data context (D-534):
// libsysutil's code, read from libsysutil.sprx (4.92) and compared with it (tests/hle/test_sysutil_cxml_compare.cpp).
// RPCS3 left all of these stubs.
//
// Each request is built as libsysutil builds it (the packet begun, a FixedMemory instance taken, a document with its
// magic) and sent on a channel to the system software; pspocket's console has none, its channels are closed
// (sysutil_cxml.hpp packets_start), so the send answers 0x8002b007 and each function answers what libsysutil answers
// then: the notification (libad, libmedi), the virtual memory card assignment (PS1/PS2 classics), the settings
// screen's abort, the AV chat's panel and muting, the monitor type of the debug video output. Two answer from what
// pspocket's console has instead, where libsysutil would read the system's answer: _cellSysutilGetSystemParamInt (the
// system parameters cellSysutilGetSystemParamInt answers), and cellOskDialogLoadAsyncExt (pspocket's on-screen
// keyboard, cellOskDialog.cpp, which cellOskDialogLoadAsync opens: the extension's load mode is the system's choice).
// libsysutil's five unnamed exports for its sub-libraries are its code too: base64 both ways and the AV chat panel's
// show state (the AV chat's), the save data's packet and state.

#include <format>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"
#include "sysutil.hpp"
#include "sysutil_cxml.hpp"

namespace pspocket::hle::sysutil {

namespace {

constexpr std::string_view kChan = "cellSysutil";

enum : u32 {
    CELL_SYSUTIL_ERROR_VALUE = 0x8002b102,
    CELL_GAMEDATA_ERROR_PARAM = 0x8002b604,
    CELL_AVC_ERROR_UNKNOWN = 0x8002b701,
    CELL_AVC_ERROR_INVALID_ARGUMENT = 0x8002b705,
    CELL_AVC_ERROR_INVALID_STATUS = 0x8002b70a,
    CELL_EFAULT = 0x8001000D,
};
constexpr u32 kNidOskDialogLoadAsync = 0x7FCFC915;

constexpr s32 err(u32 code) { return static_cast<s32>(code); }

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

// A request of libsysutil's: the packet begun (its answer not looked at) on `channel` with `id`, FixedMemory `channel`
// taken, a document on the stack with the allocator and `magic`. The functions below end it in their own orders.
struct Request {
    Request(PPUContext& c, u32 ch, s32 packet, std::string_view magic)
        : ctx(c), channel(ch), id(packet), frame(c, guest_base(c), 0x80)
    {
        packet_begin(ctx, channel, id);
        memory = fixed_memory_begin(ctx, static_cast<s32>(channel));
        doc = frame.addr();
        document_construct(ctx, doc);
        document_use_fixed_memory(ctx, doc, memory);
        document_set_magic(ctx, doc, magic);
    }

    s32 send(std::string_view what)
    {
        PacketWriter w(ctx, static_cast<s32>(channel), id, doc);
        const s32 r = document_write(ctx, doc, w);
        if (r != 0) log_handover(ctx, what, r);
        return r;
    }

    void end()
    {
        fixed_memory_end(ctx, static_cast<s32>(channel));
        packet_end(ctx, channel, id);
    }

    PPUContext& ctx;
    u32 channel;
    s32 id;
    StackVar frame;
    u32 doc = 0, memory = 0;
    Element elem;
};

// ------------------------------------------------------------------------------------------------- system parameters

// libsysutil's integer system parameters: those it lets through after the system's answer.
bool known_param(s32 id)
{
    if (id < 0x158) {
        if (id < 0x151) {
            if (id < 0x118) return id >= 0x114 || static_cast<u32>(id - 0x111) <= 1;
            return (id >= 0x121 && id <= 0x125) || id == 0x141;
        }
        return true;
    }
    if (id < 0x1024) return id >= 0x1021 || (id >= 0x1001 && (id <= 0x1007 || id == 0x1013));
    return (id >= 0x1030 && id <= 0x103B) || static_cast<u32>(id - 0x2101) <= 2;
}

// _cellSysutilGetSystemParamInt(id, value): libsysutil asks the system ("get_sysparam" on channel 0, packet 0x101)
// and lets the parameters it knows through. pspocket's console answers its own (cellSysutilGetSystemParamInt's table:
// the same answer that function gives); one libsysutil knows and pspocket's console has not is answered
// CELL_SYSUTIL_ERROR_VALUE, logged.
s32 _cellSysutilGetSystemParamInt(PPUContext& ctx, s32 id, u64 value)
{
    if (!value) return err(CELL_SYSUTIL_ERROR_VALUE);
    if (!known_param(id)) return err(CELL_SYSUTIL_ERROR_VALUE);
    s32 v = 0;
    if (!sysutil_system_param_int(id, v)) {
        log_call_site(ctx, guest_base(ctx),
            std::format("_cellSysutilGetSystemParamInt({:#x}): a parameter pspocket's console does not have: "
                        "CELL_SYSUTIL_ERROR_VALUE",
                id));
        return err(CELL_SYSUTIL_ERROR_VALUE);
    }
    if (!guest_readable(thread_of(ctx), static_cast<u32>(value), 4)) {
        log_call_site(ctx, guest_base(ctx), "_cellSysutilGetSystemParamInt: the value's word is not guest memory");
        return err(CELL_EFAULT);
    }
    ps_write32(guest_base(ctx), static_cast<u32>(value), static_cast<u32>(v));
    return 0;
}

// ------------------------------------------------------------------------------------------------- commands

// libsysutil's command (FUN_00004a10) on channel `channel`, packet 1, magic "CMND": an element `name` with the given
// int and string attributes and files, sent; then the reply's "result" read. Its answer is the send's: the reply only
// fills the document.
struct CommandFile {
    std::string_view name;
    u32 data, size;
};

s32 command(PPUContext& ctx, u32 channel, std::string_view name, std::span<const CommandFile> files,
    std::string_view what)
{
    packet_begin(ctx, channel, 1);
    const u32 memory = fixed_memory_begin(ctx, static_cast<s32>(channel));
    StackVar frame(ctx, guest_base(ctx), 0x80);
    const u32 d = frame.addr();
    document_construct(ctx, d);
    document_use_fixed_memory(ctx, d, memory);
    document_set_magic(ctx, d, "CMND");
    Element e;
    s32 r = document_create_element(ctx, d, name, static_cast<s32>(files.size()), e);
    for (usize i = 0; r == 0 && i < files.size(); i++) {
        File f;
        r = document_register_file(ctx, d, files[i].data, files[i].size, f);
        if (r == 0) r = util_set_file(ctx, e, files[i].name, f);
    }
    if (r == 0) {
        PacketWriter w(ctx, static_cast<s32>(channel), 1, d);
        r = document_write(ctx, d, w);
        if (r != 0) {
            log_handover(ctx, what, r);
        } else {
            document_clear(ctx, d);
            document_construct(ctx, d);
            StackVar done(ctx, guest_base(ctx), 16);
            const s32 read = packet_read(ctx, channel, 1, memory, 0x6000, done.addr());
            if (read >= 0 && (ps_read8(guest_base(ctx), done.addr()) || read != 0x6000)) {
                document_create_from_buffer(ctx, d, memory, 0x6000, 0);
                s32 result = 0;
                util_get_int(ctx, document_element(ctx, d), "result", result);
            }
        }
    }
    document_clear(ctx, d);
    fixed_memory_end(ctx, static_cast<s32>(channel));
    packet_end(ctx, channel, 1);
    return r;
}

// cellSysutilNotification(data): "notification" on channel 0 with the 0x110 bytes of the notification as "param".
s32 cellSysutilNotification(PPUContext& ctx, u32 data)
{
    packets_start(ctx);
    return guarded(ctx, "cellSysutilNotification", [&] {
        const CommandFile files[] = {{"param", data, 0x110}};
        return command(ctx, 0, "notification", files, "cellSysutilNotification");
    });
}

// cellSysutilGameDataAssignVmc(port, file name): a virtual memory card (port 0 or 1, a name of at most 127 bytes, none
// for an empty one) for a PS1/PS2 classic: "vmcAssign" on channel 1 (packet 0x10A, magic "GDAT"), its reply's
// "elmResult" the answer.
s32 cellSysutilGameDataAssignVmc(PPUContext& ctx, u32 port, u32 name)
{
    packets_start(ctx);
    return guarded(ctx, "cellSysutilGameDataAssignVmc", [&] {
        Mem m(ctx);
        if (port > 1 || (name && m.strlen(name) > 127)) return err(CELL_GAMEDATA_ERROR_PARAM);
        Request q(ctx, 1, 0x10A, "GDAT");
        s32 r = document_create_element(ctx, q.doc, "vmcAssign", 2, q.elem);
        if (r == 0) r = util_set_int(ctx, q.elem, "elmVmcPort", static_cast<s32>(port));
        if (r == 0) {
            r = name ? util_set_string(ctx, q.elem, "elmVmcFileName", name)
                     : util_set_string(ctx, q.elem, "elmVmcFileName", std::string_view{});
        }
        if (r == 0) r = q.send("cellSysutilGameDataAssignVmc");
        if (r == 0) {
            document_clear(ctx, q.doc);
            document_construct(ctx, q.doc);
            r = packet_read(ctx, 1, 0x10A, q.memory, 0x6000, 0);
            if (r >= 0) {
                document_create_from_buffer(ctx, q.doc, q.memory, 0x6000, 0);
                s32 result = 0;
                r = util_get_int(ctx, document_element(ctx, q.doc), "elmResult", result);
                if (r == 0) r = result;
            }
        }
        document_clear(ctx, q.doc);
        q.end();
        return r;
    });
}

// cellSysconfAbort(): "abort" on channel 0 (packet 0x110, magic "SYSC"), its reply's "result" the answer; a
// successful one clears libsysutil's record of the open settings screen (pspocket's cellSysconfOpen keeps none: its
// screen answers at once, sysutil_misc.cpp).
s32 cellSysconfAbort(PPUContext& ctx)
{
    packets_start(ctx);
    return guarded(ctx, "cellSysconfAbort", [&] {
        Request q(ctx, 0, 0x110, "SYSC");
        s32 r = document_create_element(ctx, q.doc, "abort", 0, q.elem);
        if (r == 0) r = q.send("cellSysconfAbort");
        if (r == 0) {
            document_clear(ctx, q.doc);
            document_construct(ctx, q.doc);
            StackVar done(ctx, guest_base(ctx), 16);
            r = packet_read(ctx, 0, 0x110, q.memory, 0x6000, done.addr());
            if (r >= 0) {
                if (!ps_read8(guest_base(ctx), done.addr()) && r == 0x6000) {
                    r = err(PACKET_ERROR_TRUNCATED);
                } else {
                    document_create_from_buffer(ctx, q.doc, q.memory, 0x6000, 0);
                    s32 result = 0;
                    r = util_get_int(ctx, document_element(ctx, q.doc), "result", result);
                    if (r == 0 && result < 0) r = result;
                }
            }
        }
        document_clear(ctx, q.doc);
        q.end();
        return r;
    });
}

// cellOskDialogLoadAsyncExt(container, dialog param, input field info): libsysutil's cellOskDialogLoadAsync with the
// load mode 1 for the system. pspocket's on-screen keyboard is the same for both.
s32 cellOskDialogLoadAsyncExt(PPUContext& ctx, u32 container, u32 dialog_param, u32 field_info)
{
    const Function* f = registry().find("cellSysutil", kNidOskDialogLoadAsync);
    if (!f) PS_FATAL(kChan, "cellOskDialogLoadAsync is not pspocket's");
    ctx.gpr[3] = container;
    ctx.gpr[4] = dialog_param;
    ctx.gpr[5] = field_info;
    f->fn(ctx, guest_base(ctx));
    return static_cast<s32>(static_cast<u32>(ctx.gpr[3]));
}

// ------------------------------------------------------------------------------------------------- AV chat

// libsysutil's AV chat requests on channel 0 (packet 0x10B, magic "AVCH"), for a loaded AV chat; a video one (its
// panel, its video) for one loaded for video. A failed request answers CELL_AVC_ERROR_UNKNOWN.
s32 avc_request(PPUContext& ctx, std::string_view what, bool needs_video, std::string_view element,
    std::initializer_list<std::pair<std::string_view, s32>> ints)
{
    u32 media = 0;
    if (!sysutil_avc_loaded(media) || (needs_video && media == 1)) return err(CELL_AVC_ERROR_INVALID_STATUS);
    packets_start(ctx);
    return guarded(ctx, what, [&] {
        Request q(ctx, 0, 0x10B, "AVCH");
        s32 r = document_create_element(ctx, q.doc, element, static_cast<s32>(ints.size()), q.elem);
        for (const auto& [name, value] : ints)
            if (r == 0) r = util_set_int(ctx, q.elem, name, value);
        if (r == 0) r = q.send(what);
        document_clear(ctx, q.doc);
        q.end();
        return r == 0 ? 0 : err(CELL_AVC_ERROR_UNKNOWN);
    });
}

// The panel's show state with a transition (libsysutil's export 0x44F288A7 takes the transition; the panel functions
// give -1).
s32 avc_show_state(PPUContext& ctx, std::string_view what, s32 transition, u8 show)
{
    if (transition > 4) return err(CELL_AVC_ERROR_INVALID_ARGUMENT);
    return avc_request(ctx, what, true, "AvcSetShowState", {{"transition_type", transition}, {"showstate", show}});
}

s32 cellSysutilAvcShowPanel(PPUContext& ctx) { return avc_show_state(ctx, "cellSysutilAvcShowPanel", -1, 1); }
s32 cellSysutilAvcHidePanel(PPUContext& ctx) { return avc_show_state(ctx, "cellSysutilAvcHidePanel", -1, 0); }

s32 cellSysutilAvcSetVoiceMuting(PPUContext& ctx, u8 muting)
{
    return avc_request(ctx, "cellSysutilAvcSetVoiceMuting", false, "AvcSetVoiceMute", {{"voicemute", muting}});
}

s32 cellSysutilAvcSetVideoMuting(PPUContext& ctx, u8 muting)
{
    return avc_request(ctx, "cellSysutilAvcSetVideoMuting", true, "AvcSetVideoMute", {{"videomute", muting}});
}

// ------------------------------------------------------------------------------------------------- outputs

// The video and audio outputs' callbacks: one each, kept by libsysutil for the system's output events (which pspocket's
// console never sends). The slot is not looked at.
s32 set_callback(PPUContext& ctx, std::string_view what, u32 at, u32 func, u32 userdata)
{
    return guarded(ctx, what, [&] {
        Mem m(ctx);
        const u32 g = globals(ctx);
        m.w32(g + at + 4, userdata);
        m.w32(g + at, func);
        return 0;
    });
}

s32 cellVideoOutRegisterCallback(PPUContext& ctx, u32, u32 func, u32 userdata)
{
    return set_callback(ctx, "cellVideoOutRegisterCallback", glob::video_out_cb, func, userdata);
}
s32 cellVideoOutUnregisterCallback(PPUContext& ctx, u32)
{
    return set_callback(ctx, "cellVideoOutUnregisterCallback", glob::video_out_cb, 0, 0);
}
s32 cellAudioOutRegisterCallback(PPUContext& ctx, u32, u32 func, u32 userdata)
{
    return set_callback(ctx, "cellAudioOutRegisterCallback", glob::audio_out_cb, func, userdata);
}
s32 cellAudioOutUnregisterCallback(PPUContext& ctx, u32)
{
    return set_callback(ctx, "cellAudioOutUnregisterCallback", glob::audio_out_cb, 0, 0);
}

// cellVideoOutDebugSetMonitorType(a, b): libsysutil's "set" of system parameter 0x4B09 with a 12-byte payload {a, 0,
// b} (channel 0, packet 0x101, magic "PARA": an element with the ID and a "param" child holding the payload as a
// file), its reply's result the answer.
s32 cellVideoOutDebugSetMonitorType(PPUContext& ctx, u32 a, u32 b)
{
    packets_start(ctx);
    return guarded(ctx, "cellVideoOutDebugSetMonitorType", [&] {
        Mem m(ctx);
        packet_begin(ctx, 0, 0x101);
        const u32 memory = fixed_memory_begin(ctx, 0);
        StackVar payload(ctx, m.base(), 0x1D0);
        m.w32(payload.addr(), a);
        m.w32(payload.addr() + 4, 0);
        m.w32(payload.addr() + 8, b);
        s32 r = 0;
        {
            StackVar frame(ctx, m.base(), 0x80);
            const u32 d = frame.addr();
            document_construct(ctx, d);
            document_use_fixed_memory(ctx, d, memory);
            document_set_magic(ctx, d, "PARA");
            Element set, param;
            r = document_create_element(ctx, d, "set", 1, set);
            if (r == 0) r = util_set_int(ctx, set, "id", 0x4B09);
            if (r == 0) r = document_create_element(ctx, d, "param", 2, param);
            if (r == 0) r = element_append_child(ctx, set, param);
            File f;
            if (r == 0) r = document_register_file(ctx, d, payload.addr(), 12, f);
            if (r == 0) r = util_set_file(ctx, param, "payload", f);
            if (r == 0) {
                PacketWriter w(ctx, 0, 0x101, d);
                r = document_write(ctx, d, w);
                if (r != 0) log_handover(ctx, "cellVideoOutDebugSetMonitorType", r);
            }
            document_clear(ctx, d);
        }
        if (r == 0) {
            // The reply (FUN_00012a0c): its result, then its "param" child's payload, at most 12 bytes.
            m.fill(payload.addr(), 0, 0x1D0);
            StackVar frame(ctx, m.base(), 0x80);
            const u32 d = frame.addr();
            document_construct(ctx, d);
            StackVar done(ctx, m.base(), 16);
            s32 result = 0;
            r = packet_read(ctx, 0, 0x101, memory, 0x6000, done.addr());
            if (r < 0) {
                sysutil_printf(ctx, "packet read error %x.\n", {static_cast<u64>(static_cast<s64>(r))});
            } else if (!m.r8(done.addr()) && r == 0x6000) {
                r = err(PACKET_ERROR_TRUNCATED);
                sysutil_printf(ctx, "not enough receive buffer.\n", {});
            } else {
                document_create_from_buffer(ctx, d, memory, 0x6000, 0);
                const Element root = document_element(ctx, d);
                r = util_get_int(ctx, root, "result", result);
                if (r != 0) {
                    sysutil_printf(ctx, "get result failed %x\n", {static_cast<u64>(static_cast<s64>(r))});
                } else {
                    const Element p = util_find_child(ctx, root, "param");
                    File f;
                    if (!p.doc || static_cast<s32>(p.off) < 0) {
                        r = err(PACKET_ERROR_NO_PARAM);
                    } else if ((r = util_get_file(ctx, p, "payload", f)) == 0) {
                        // The payload copied back: at most the 12 bytes asked for.
                        if (f.size == 0 || f.size > 12) r = err(CELL_SYSUTIL_ERROR_VALUE);
                        else m.copy(payload.addr(), file_address(ctx, f), f.size);
                    }
                }
            }
            document_clear(ctx, d);
            if (r == 0) r = result;
        }
        fixed_memory_end(ctx, 0);
        packet_end(ctx, 0, 0x101);
        return r;
    });
}

// ------------------------------------------------------------------------------------------------- unnamed exports

// libsysutil's helpers for its sub-libraries, exported without names (RPCS3 lists none of them).

constexpr std::string_view kBase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 0x987D910E (the AV chat's): `size` bytes of `src` in base64 to `dst`, CR LF after every 76 characters, then '='
// until the characters written (the line breaks counted) reach a multiple of four, which gives a whole last group
// four of them, and a NUL. The characters written, the NUL counted.
u32 cellSysutil_987D910E(PPUContext& ctx, u32 src, u32 size, u32 dst)
{
    return static_cast<u32>(guarded(ctx, "cellSysutil_987D910E", [&] {
        Mem m(ctx);
        u32 n = 0, line = 0, pos = 0, i = 0;
        const auto line_break = [&] {
            m.w8(dst + pos, 13);
            m.w8(dst + pos + 1, 10);
            pos += 2;
            n += 2;
            line = 0;
        };
        if (size != 0) {
            u8 mask = 0x80;
            do {
                u32 sextet = 0;
                s32 bit = 5;
                for (u32 k = 0; k < 6; k++) {
                    const u8 here = mask;
                    mask = here >> 1;
                    if (m.r8(src + i) & here) sextet |= 1u << bit;
                    bit--;
                    if (mask == 0) {
                        i++;
                        if (size <= i) break;
                        mask = 0x80;
                    }
                }
                m.w8(dst + pos, static_cast<u8>(kBase64[sextet & 0x3F]));
                line++;
                pos++;
                n++;
                if (line > 75) line_break();
            } while (i < size);
        }
        u32 pad = 4 - (n & 3);
        for (;;) {
            pad--;
            if ((pad & 0xFF) == 0xFF) break;
            const bool wrap = line > 75;
            m.w8(dst + pos, '=');
            pos++;
            n++;
            if (wrap) line_break();
        }
        m.w8(dst + pos, 0);
        return static_cast<s32>(n + 1);
    }));
}

// 0x1D58430F: base64 at `src` decoded to `dst`, up to '=', the NUL or past `max` characters (CR and LF skipped); a
// character outside base64 answers 0. The bytes written.
u32 cellSysutil_1D58430F(PPUContext& ctx, u32 src, u32 max, u32 dst)
{
    return static_cast<u32>(guarded(ctx, "cellSysutil_1D58430F", [&] {
        Mem m(ctx);
        u32 i = 0, out = 0, pos = 0, acc = 0;
        s32 bit = 7;
        s8 c = static_cast<s8>(m.r8(src));
        while (c != '=') {
            if (c == 0 || i > max) break;
            if (c != '\n' && c != '\r') {
                const usize v = kBase64.find(static_cast<char>(c));
                if (c < 0 || v == std::string_view::npos) return 0;
                u8 mask = 0x20;
                for (u32 k = 0; k < 6; k++) {
                    const u8 here = mask;
                    mask = here >> 1;
                    u32 value = (here & v) ? 1u << bit : acc;
                    bit--;
                    value |= acc;
                    acc = value;
                    if (bit < 0) {
                        m.w8(dst + pos, static_cast<u8>(value));
                        pos++;
                        out++;
                        bit = 7;
                        acc = 0;
                    }
                }
            }
            i++;
            c = static_cast<s8>(m.r8(src + i));
        }
        return static_cast<s32>(out);
    }));
}

// 0x44F288A7 (the AV chat's): the panel's show state with a transition (-1 to 4), as the panel functions send it.
s32 cellSysutil_44F288A7(PPUContext& ctx, s32 transition, u8 show)
{
    return avc_show_state(ctx, "cellSysutil_44F288A7", transition, show);
}

// 0xADEF7C02 (the save data's): with 0, the packet on channel 1 begun for packet 0x104 (its answer not looked at)
// and FixedMemory 1 taken; the FixedMemory taken last, either way. libsysutil's save data ends it; pspocket's save
// data (cellSaveData.cpp) has no use for it.
u32 cellSysutil_ADEF7C02(PPUContext& ctx, s32 begin)
{
    packets_start(ctx);
    return static_cast<u32>(guarded(ctx, "cellSysutil_ADEF7C02", [&] {
        Mem m(ctx);
        const u32 at = globals(ctx) + glob::savedata_memory;
        if (begin == 0) {
            packet_begin(ctx, 1, 0x104);
            m.w32(at, fixed_memory_begin(ctx, 1));
        }
        return static_cast<s32>(m.r32(at));
    }));
}

// 0x340F280B (the save data's): libsysutil's save data state, read under its save data lock (an uncontended
// lightweight mutex: no lv2 call). libsysutil's own save data code sets it; pspocket's (cellSaveData.cpp) does not,
// so it stays 0.
s32 cellSysutil_340F280B(PPUContext& ctx)
{
    return guarded(ctx, "cellSysutil_340F280B", [&] {
        Mem m(ctx);
        return static_cast<s32>(m.r32(globals(ctx) + glob::savedata_state));
    });
}

} // namespace

void register_system(Registry& r)
{
    r.add("cellSysutil", {
        PS_HLE_FUNC(0xA297525E, _cellSysutilGetSystemParamInt),
        PS_HLE_FUNC(0x230B2925, cellSysutilNotification),
        PS_HLE_FUNC(0x9997E6B6, cellSysutilGameDataAssignVmc),
        PS_HLE_FUNC(0x76948BFC, cellSysconfAbort),
        PS_HLE_FUNC(0xE64582B3, cellOskDialogLoadAsyncExt),
        PS_HLE_FUNC(0x2DBC92C7, cellSysutilAvcShowPanel),
        PS_HLE_FUNC(0xD9EA5709, cellSysutilAvcHidePanel),
        PS_HLE_FUNC(0x4056C932, cellSysutilAvcSetVoiceMuting),
        PS_HLE_FUNC(0x71ACB8D3, cellSysutilAvcSetVideoMuting),
        PS_HLE_FUNC(0x8E8BC444, cellVideoOutRegisterCallback),
        PS_HLE_FUNC(0x7871BED4, cellVideoOutUnregisterCallback),
        PS_HLE_FUNC(0x68BC4FF3, cellAudioOutRegisterCallback),
        PS_HLE_FUNC(0x9DFDAD46, cellAudioOutUnregisterCallback),
        PS_HLE_FUNC(0xCFDF24BB, cellVideoOutDebugSetMonitorType),
        PS_HLE_FUNC(0x987D910E, cellSysutil_987D910E),
        PS_HLE_FUNC(0x1D58430F, cellSysutil_1D58430F),
        PS_HLE_FUNC(0x44F288A7, cellSysutil_44F288A7),
        PS_HLE_FUNC(0xADEF7C02, cellSysutil_ADEF7C02),
        PS_HLE_FUNC(0x340F280B, cellSysutil_340F280B),
    });
    // RPCS3's name for the save data functions' callback structures (a hidden variable there): pspocket's
    // cellSaveData keeps them in it (D-534).
    r.add_variables("cellSysutil", {
        Variable{{}, "g_savedata_context", recomp::nid_of("g_savedata_context"), 2128, 0x100, nullptr},
    });
}

} // namespace pspocket::hle::sysutil
