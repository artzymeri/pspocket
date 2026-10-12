#pragma once

// libsysutil's sub-libraries (D-925): the modules beside libsysutil.sprx in /dev_flash/sys/external (the background
// downloads, the RTC alarm, the PS1 classics' settings, remote play, the game launcher, printing, the video player,
// the PSP-to-PS3 movie converter, DTCP-IP) and libfs_utility_init in sys/internal. Read from Sony's modules (4.92) and
// compared with them call for call (tests/hle/test_sysutil_more.cpp).
//
// Most of them are front ends of the system software: each function builds a request as a cxml document in one of
// libsysutil's FixedMemory instances, sends it on one of libsysutil's packet channels (sysutil_cxml.hpp) and reads the
// system's reply into the same memory. pspocket's console has no system software and its channels are closed (D-534):
// a request runs the module's code to its handover and gets the closed channel's answer there, 0x8002b007, logged once
// per kind of request. pspocket's functions do what Sony's do, through pspocket's own libsysutil (the same calls, the
// same documents, the same lv2 calls), and read the reply as Sony's code reads it, so they answer as Sony's modules
// would on a console that answered.

#include <cstring>
#include <format>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"
#include "sysutil_cxml.hpp"

namespace pspocket::hle {
class Registry;
}

namespace pspocket::hle::sysutil::sub {

inline constexpr u32 CELL_EFAULT = 0x8001000D;
inline constexpr u32 CELL_EINVAL = 0x80010002;

constexpr s32 err(u32 code) { return static_cast<s32>(code); }

// libsysutil's packets and FixedMemory taken for one request on `channel` with the packet `id` and given back, each
// step where the module takes it; after a fault, whatever was taken and not given back is, and the request's document
// cleared, so its tables go back to FixedMemory (abandon).
class Request {
public:
    Request(PPUContext& ctx, u32 channel, s32 id) : ctx_(ctx), channel_(channel), id_(id) {}
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    u32 channel() const { return channel_; }
    s32 id() const { return id_; }
    u32 memory() const { return memory_; }

    // cellSysutilPacketBegin (its answer not looked at).
    void begin_packet()
    {
        packet_begin(ctx_, channel_, id_);
        packet_ = true;
    }
    // cellSysutilPacketBegin, its answer looked at: the packet is the request's when it is 0.
    s32 try_begin_packet()
    {
        const s32 r = packet_begin(ctx_, channel_, id_);
        if (r == 0) packet_ = true;
        return r;
    }
    // FixedMemory::Begin(channel): the instance, or 0 when it is in use.
    u32 begin_memory()
    {
        memory_ = fixed_memory_begin(ctx_, static_cast<s32>(channel_));
        memory_taken_ = true;
        return memory_;
    }
    void end_memory()
    {
        memory_taken_ = false;
        fixed_memory_end(ctx_, static_cast<s32>(channel_));
    }
    void end_packet()
    {
        packet_ = false;
        packet_end(ctx_, channel_, id_);
    }
    // After a fault (the document's frame was given back by the unwinding, but nothing has run on the stack since).
    void abandon()
    {
        if (doc_) document_clear(ctx_, doc_);
        if (memory_taken_) end_memory();
        if (packet_) end_packet();
    }

    // A document in FixedMemory (the module's on its stack): constructed, its allocator FixedMemory::Allocate on this
    // request's instance, its header's magic.
    void prepare(u32 doc, std::string_view magic)
    {
        doc_ = doc;
        document_construct(ctx_, doc);
        document_use_fixed_memory(ctx_, doc, memory_);
        document_set_magic(ctx_, doc, magic);
    }
    // ~Document() then Document(): the request's tables given back, the document empty for the reply (no allocator).
    void recycle(u32 doc)
    {
        document_clear(ctx_, doc);
        document_construct(ctx_, doc);
        doc_ = doc;
    }
    // ~Document(): Clear (a document made over the reply keeps its pointers, as libsysutil leaves them).
    void destroy(u32 doc)
    {
        document_clear(ctx_, doc);
        if (doc == doc_) doc_ = 0;
    }

    // Document::Write through a PacketWriter on this channel and packet: the handover to the system software. A failure
    // is logged once per kind of request (`what`).
    s32 send(u32 doc, std::string_view what)
    {
        PacketWriter w(ctx_, static_cast<s32>(channel_), id_, doc);
        const s32 r = document_write(ctx_, doc, w);
        if (r != 0) log_handover(ctx_, what, r);
        return r;
    }

    // cellSysutilPacketRead of the reply into this request's FixedMemory (0x6000 bytes), `done` a guest byte or 0.
    s32 read(u32 done) { return packet_read(ctx_, channel_, id_, memory_, 0x6000, done); }
    // cellSysutilPacketRead into a buffer of the module's.
    s32 read_into(u32 buffer, u32 size, u32 done) { return packet_read(ctx_, channel_, id_, buffer, size, done); }

private:
    PPUContext& ctx_;
    u32 channel_;
    s32 id_;
    u32 memory_ = 0, doc_ = 0;
    bool packet_ = false, memory_taken_ = false;
};

// An argument of a module's diagnostic printed through liblv2's printf: an integer (as the module passes it in its
// register), or a string of the module's (put on the stack for the call).
struct PrintArg {
    u64 value = 0;
    std::string_view text;
    bool is_text = false;
    PrintArg(u64 v) : value(v) {}
    PrintArg(u32 v) : value(v) {}
    PrintArg(s32 v) : value(static_cast<u64>(static_cast<s64>(v))) {}
    PrintArg(const char* s) : text(s), is_text(true) {}
    PrintArg(std::string_view s) : text(s), is_text(true) {}
};

// liblv2's printf (vprintf) of `fmt` with `args` (at most seven).
inline void module_printf(PPUContext& ctx, std::string_view fmt, std::initializer_list<PrintArg> args)
{
    u32 strings = 0;
    for (const PrintArg& a : args)
        if (a.is_text) strings += static_cast<u32>(a.text.size()) + 1;
    StackVar block(ctx, guest_base(ctx), strings ? strings : 1);
    u8* b = guest_base(ctx);
    u32 at = block.addr();
    u64 values[7] = {};
    u32 n = 0;
    for (const PrintArg& a : args) {
        if (n == 7) break;
        if (a.is_text) {
            std::memcpy(b + at, a.text.data(), a.text.size());
            b[at + a.text.size()] = 0;
            values[n++] = at;
            at += static_cast<u32>(a.text.size()) + 1;
        } else {
            values[n++] = a.value;
        }
    }
    switch (n) {
    case 0: sysutil_printf(ctx, fmt, {}); break;
    case 1: sysutil_printf(ctx, fmt, {values[0]}); break;
    case 2: sysutil_printf(ctx, fmt, {values[0], values[1]}); break;
    case 3: sysutil_printf(ctx, fmt, {values[0], values[1], values[2]}); break;
    case 4: sysutil_printf(ctx, fmt, {values[0], values[1], values[2], values[3]}); break;
    case 5: sysutil_printf(ctx, fmt, {values[0], values[1], values[2], values[3], values[4]}); break;
    case 6: sysutil_printf(ctx, fmt, {values[0], values[1], values[2], values[3], values[4], values[5]}); break;
    default:
        sysutil_printf(ctx, fmt, {values[0], values[1], values[2], values[3], values[4], values[5], values[6]});
        break;
    }
}

// A function of libsysutil's (pspocket's own, cellSysutil.cpp) called as the modules call it: its answer.
inline s32 call_sysutil(PPUContext& ctx, std::string_view module, std::string_view name, std::initializer_list<u64> args)
{
    const Function* f = registry().find(module, recomp::nid_of(name));
    if (!f) PS_FATAL("sysutil", "{}::{} is not pspocket's", module, name);
    u64 saved[8];
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 0;
    for (const u64 a : args) ctx.gpr[3 + i++] = a;
    f->fn(ctx, guest_base(ctx));
    const s32 r = static_cast<s32>(static_cast<u32>(ctx.gpr[3]));
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

// The descriptor of a module's internal function (the dispatchers the modules register with libsysutil), linked when
// the loader has not.
inline u32 internal_descriptor(std::string_view module, std::string_view name)
{
    HleProcess* p = HleProcess::current();
    const u32 nid = recomp::nid_of(name);
    if (const u32 d = p->descriptor_of(module, nid)) return d;
    return p->link_function(module, nid, name);
}

// A cxml document object on the calling thread's stack (0x74 bytes; the modules keep theirs in their frames).
class StackDocument {
public:
    explicit StackDocument(PPUContext& ctx) : frame_(ctx, guest_base(ctx), 0x80) {}
    u32 addr() const { return frame_.addr(); }

private:
    StackVar frame_;
};

// A call into libsysutil made as a module makes it, through libsysutil's export: guest memory that is not there ends
// the call, logged with the call site, and it answers `on_fault`, as pspocket's exports answer it (cellSysutilCxml.cpp,
// whose guarded() catches what libsysutil itself would fault on); the module goes on. (Sony's module run on pspocket's
// libsysutil goes on the same way.)
template <class R, class F>
R as_export(PPUContext& ctx, std::string_view what, R on_fault, F&& f)
{
    try {
        return f();
    } catch (const Fault& e) {
        log_call_site(ctx, guest_base(ctx),
            std::format("{}: guest memory at {:#010x} is not there (libsysutil faults there)", what, e.addr));
        return on_fault;
    }
}

// A function body: guest memory that is not there ends it (Sony's module faults there), logged with the call site and
// answered CELL_EFAULT; `on_fault` ends what the body had begun.
template <class Body, class OnFault>
s32 guarded(PPUContext& ctx, std::string_view module, std::string_view what, Body&& body, OnFault&& on_fault)
{
    try {
        return body();
    } catch (const Fault& f) {
        try {
            on_fault();
        } catch (const Fault&) {
        }
        log_call_site(ctx, guest_base(ctx),
            std::format("{}: guest memory at {:#010x} is not there ({} faults there): CELL_EFAULT", what, f.addr,
                module));
        return err(CELL_EFAULT);
    }
}

template <class Body>
s32 guarded(PPUContext& ctx, std::string_view module, std::string_view what, Body&& body)
{
    return guarded(ctx, module, what, std::forward<Body>(body), [] {});
}

// A request's function: `body(q)` with the Request it runs on (given back after a fault), libsysutil's channels started
// first (D-534 packets_start, once per process).
template <class Body>
s32 with_request(PPUContext& ctx, std::string_view module, std::string_view what, u32 channel, s32 id, Body&& body)
{
    packets_start(ctx);
    Request q(ctx, channel, id);
    return guarded(ctx, module, what, [&] { return body(q); }, [&] { q.abandon(); });
}

// The libraries' registrations (each file's), called by register_sysutil_more.
void register_print(Registry& r);
void register_pesm(Registry& r);
void register_game_ps1emu(Registry& r);

} // namespace pspocket::hle::sysutil::sub
