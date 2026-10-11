// cellRudp against Sony's librudp (D-563, D-597, D-633): the same calls on the same inputs to pspocket's library and
// to Sony's module, both over pspocket's lv2 network (loopback sockets: each half's contexts talk to each other, each
// in its own wire format). Compared, as a game sees them: every function's answer before Init; the maximum segment
// size and its bounds; the contexts' IDs (relative to the first); every option's default in 4 and 8 bytes and the
// answers to bad values; the status structures' bytes (CellRudpContextStatus asked for 256 bytes, CellRudpStatus 1024)
// with what is past them; Bind's checks on UDP, UDP P2P and TCP sockets and the mux modes; the local and remote
// information; a connection over the internal I/O thread (Listen, Initiate, Accept), its handler events in order, the
// states, messages both ways with their read information, the sizes readable and writable, polling, the end of the
// connection; then one driven by ProcessEvents alone. What D-630 to D-632 decided without Sony's module (the
// signatures, the structures' layouts, the units, what ProcessEvents reads) shows here as differences to read Sony's
// answers off. Times and sequence numbers in the structures are masked (notes). No game data; skipped without the
// firmware (compare_util.hpp).

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include "compare_util.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;
using namespace std::chrono_literals;
namespace as = pspocket::recomp::ppu::as;

namespace {

constexpr u32 kCode = kData, kHandler = kData + 0x800, kRing = kData + 0x1000, kArgs = kData + 0x2000,
              kPeer = kData + 0x2100, kAddr = kData + 0x2200, kStatus = kData + 0x3000, kEvents = kData + 0x3800,
              kOut = kData + 0x4000, kIn = kData + 0x6000;

// The context handler (ctx_id, event_id, error_code, ring), guest code: the record {ctx, event, error} at ring + 16 +
// 12 * (n % 256), then the count n + 1 at ring + 0 (test_rudp.cpp's).
void write_handler(u8* b)
{
    const std::vector<u32> c = {as::lwz(7, 0, 6), as::andi_(8, 7, 255), as::mulli(8, 8, 12), as::add(8, 8, 6),
        as::stw(3, 16, 8), as::stw(4, 20, 8), as::stw(5, 24, 8), as::lwsync(), as::addi(7, 7, 1), as::stw(7, 0, 6),
        as::li(3, 0), as::blr()};
    for (u32 i = 0; i < c.size(); i++) ps_write32(b, kCode + 4 * i, c[i]);
    ps_write32(b, kHandler, kCode);
    ps_write32(b, kHandler + 4, 0);
    ps_write32(b, kRing, 0);
}

// lv2's call `number` on the half's thread.
s32 sc(GuestThread& g, u32 number, std::initializer_list<u64> args)
{
    u32 i = 3;
    for (const u64 a : args) g.ctx.gpr[i++] = a;
    g.ctx.gpr[11] = number;
    g.f.kernel->syscall(g.ctx);
    return static_cast<s32>(g.ctx.gpr[3]);
}

void sockaddr(u8* b, u32 at, u16 port, u16 vport = 0, u32 addr = 0x7F000001)
{
    std::memset(b + at, 0, 16);
    ps_write8(b, at, 16);
    ps_write8(b, at + 1, 2);
    ps_write16(b, at + 2, port);
    ps_write32(b, at + 4, addr);
    ps_write16(b, at + 8, vport);
}

struct Run : Half<> {
    s32 first = 0; // the first context's ID: the others are written relative to it

    Run(const Setup& s, bool sony_half) : Half(s, sony_half, "cellRudp") {}

    std::string id_text(u64 r3) const
    {
        const s32 v = static_cast<s32>(r3);
        if (v < 0) return answer(r3);
        return std::format("first + {}", v - first);
    }

    // A socket: UDP at 127.0.0.1:port, UDP P2P at SCE_NP_PORT and `vport`, or TCP.
    s32 socket(s32 type, u16 port, u16 vport = 0)
    {
        const s32 s = sc(t, 713, {2, static_cast<u64>(type), 0});
        if (s >= 0 && type != 1) {
            if (type == 6) sockaddr(b, kAddr, 3658, vport, 0);
            else sockaddr(b, kAddr, port);
            sc(t, 701, {static_cast<u64>(s), kAddr, 16});
        }
        return s;
    }

    // CreateContext: its answer, and the ID it wrote (relative to the first; all ones without one).
    s32 create(const std::string& key, u32 handler = kHandler)
    {
        ps_write32(b, kArgs, 0xFFFFFFFF);
        const u64 r = this->r("cellRudpCreateContext", {handler, kRing, kArgs}, key);
        const s32 id = static_cast<s32>(ps_read32(b, kArgs));
        if (!first && !static_cast<s32>(r)) first = id;
        o.line(key + ": the ID", static_cast<s32>(r) ? std::string("(none)") : id_text(static_cast<u32>(id)));
        return id;
    }

    void set(s32 ctx, s32 option, u32 value, const std::string& key)
    {
        ps_write32(b, kArgs + 0x40, value);
        r("cellRudpSetOption", {static_cast<u64>(ctx), static_cast<u64>(option), kArgs + 0x40, 4}, key);
    }

    // GetOption in `len` bytes: the answer and the bytes written (over 0xEE).
    void option(s32 ctx, s32 option, u32 len, const std::string& key)
    {
        std::memset(b + kArgs + 0x48, 0xEE, 8);
        r("cellRudpGetOption", {static_cast<u64>(ctx), static_cast<u64>(option), kArgs + 0x48, len}, key);
        o.line(key + ": the value", hex(b + kArgs + 0x48, 8));
    }

    // A context's status, 256 bytes asked for: the answer and the bytes (over 0xEE); its state word on its own.
    u32 status(s32 ctx, const std::string& key, bool bytes = true)
    {
        std::memset(b + kStatus, 0xEE, 256);
        const u64 a = side.call("cellRudpGetContextStatus", {static_cast<u64>(ctx), kStatus, 256}).value_or(~0ull);
        if (bytes) {
            o.line(key + ": GetContextStatus", answer(a));
            o.line(key + ": its state and parentId", hex(b + kStatus, 8));
            o.note(key + ": the context status " + hex(b + kStatus, 256));
            o.line(key + ": past 48 bytes", hex(b + kStatus + 48, 208));
        }
        return ps_read32(b, kStatus);
    }

    // The handler's events since `from` (ctx relative to the first, event, error), one line.
    std::string events(u32 from)
    {
        const u32 n = load32_acquire(b, kRing);
        std::string s;
        for (u32 i = from; i < n && i < from + 64; i++) {
            const u32 at = kRing + 16 + 12 * (i % 256);
            s += std::format("{}[{} {} {:#x}]", s.empty() ? "" : " ", id_text(ps_read32(b, at)), ps_read32(b, at + 4),
                ps_read32(b, at + 8));
        }
        return s.empty() ? "(none)" : s;
    }

    template <class F>
    bool within(std::chrono::milliseconds limit, F&& done)
    {
        const auto until = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < until) {
            if (done()) return true;
            std::this_thread::sleep_for(1ms);
        }
        return done();
    }

    // A non-blocking Read, again until there is something when `wait` (up to 5 s; `pump`: ProcessEvents(0) in
    // between): the answer, the bytes, the read information (its first 6 bytes compared; the sequence number and
    // the time stamp a note).
    void read(s32 ctx, u32 len, const std::string& key, bool pump, bool wait = true)
    {
        u64 a = 0;
        std::memset(b + kIn, 0xEE, 16);
        within(wait ? 5000ms : 0ms, [&] {
            if (pump) side.call("cellRudpProcessEvents", {0});
            a = side.call("cellRudpRead", {static_cast<u64>(ctx), kIn + 0x100, len, 1, kIn}).value_or(~0ull);
            return static_cast<s32>(a) != static_cast<s32>(0x80770016); // WOULDBLOCK
        });
        o.line(key, answer(a));
        const s32 n = static_cast<s32>(a);
        if (n > 0) o.line(key + ": the data", hex(b + kIn + 0x100, static_cast<usize>(std::min(n, 64))));
        o.line(key + ": the read information", hex(b + kIn, 6));
        o.note(key + ": the read information's sequence number and time stamp " + hex(b + kIn + 6, 6));
    }

    s32 write(s32 ctx, u32 size, u32 seed, const std::string& key)
    {
        for (u32 i = 0; i < size; i++) ps_write8(b, kOut + i, static_cast<u8>(seed + i * 7));
        return static_cast<s32>(r("cellRudpWrite", {static_cast<u64>(ctx), kOut, size, 0}, key));
    }
};

// A connection between two contexts of the half: a listener at vport 10 and a client at vport 20, over the I/O
// thread (`pump` false) or ProcessEvents (`pump` true); messages both ways, polling, the end.
void connection(Run& h, bool pump, u16 port)
{
    u8* b = h.b;
    const std::string w = pump ? "ProcessEvents: " : "the I/O thread: ";
    const u32 from = load32_acquire(b, kRing);
    const s32 sa = h.socket(2, port), sb = h.socket(2, static_cast<u16>(port + 1));
    const s32 server = h.create(w + "the listener"), client = h.create(w + "the client");
    for (const s32 c : {server, client}) h.set(c, 7, 1, w + "NONBLOCK"); // nothing here waits on Sony's units
    h.r("cellRudpBind", {static_cast<u64>(server), static_cast<u64>(sa), 10, 0}, w + "Bind the listener");
    h.r("cellRudpBind", {static_cast<u64>(client), static_cast<u64>(sb), 20, 0}, w + "Bind the client");
    h.r("cellRudpListen", {static_cast<u64>(server), 4}, w + "Listen");
    h.status(server, w + "the listener, listening");
    sockaddr(b, kPeer, port);
    h.r("cellRudpInitiate", {static_cast<u64>(client), kPeer, 16, 10}, w + "Initiate");
    h.o.note(w + std::format("the client's state after Initiate {}", h.status(client, "", false))); // timing
    u64 accepted = 0;
    const bool got = h.within(5000ms, [&] {
        if (pump) h.side.call("cellRudpProcessEvents", {0});
        accepted = h.side.call("cellRudpAccept", {static_cast<u64>(server)}).value_or(~0ull);
        return static_cast<s32>(accepted) >= 0;
    });
    h.o.line(w + "Accept within 5 s", got ? h.id_text(accepted) : answer(accepted));
    if (!got) return;
    const s32 conn = static_cast<s32>(accepted);
    h.within(5000ms, [&] {
        if (pump) h.side.call("cellRudpProcessEvents", {0});
        return h.status(client, "", false) == 4;
    });
    h.status(client, w + "the client, connected");
    h.status(conn, w + "the accepted context");
    h.o.line(w + "the events so far", h.events(from));
    ps_write32(b, kArgs, 16);
    std::memset(b + kAddr, 0xEE, 16);
    h.r("cellRudpGetRemoteInfo", {static_cast<u64>(conn), kAddr, kArgs, kArgs + 4}, w + "GetRemoteInfo");
    h.o.line(w + "GetRemoteInfo: the address, its length, the vport", hex(b + kAddr, 16) + " " + hex(b + kArgs, 6));
    std::memset(b + kArgs, 0xEE, 16);
    ps_write32(b, kArgs + 4, 16);
    h.r("cellRudpGetLocalInfo", {static_cast<u64>(conn), kArgs, kAddr, kArgs + 4, kArgs + 8, kArgs + 10},
        w + "GetLocalInfo");
    h.o.line(w + "GetLocalInfo: the socket relative to the listener's, the vport, the mux mode",
        std::format("{} {} {}", static_cast<s32>(ps_read32(b, kArgs)) - sa, ps_read16(b, kArgs + 8),
            ps_read8(b, kArgs + 10)));
    h.r("cellRudpGetSizeWritable", {static_cast<u64>(client)}, w + "GetSizeWritable");

    // Messages both ways.
    h.write(client, 100, 1, w + "Write 100 bytes");
    h.write(client, 1, 2, w + "Write 1 byte");
    h.write(conn, 700, 3, w + "Write 700 bytes back");
    h.within(5000ms, [&] {
        if (pump) h.side.call("cellRudpProcessEvents", {0});
        return static_cast<s32>(h.side.call("cellRudpGetSizeReadable", {static_cast<u64>(conn)}).value_or(0)) >= 101;
    });
    h.r("cellRudpGetSizeReadable", {static_cast<u64>(conn)}, w + "GetSizeReadable");
    h.r("cellRudpRead", {static_cast<u64>(conn), kIn, 50, 1, 0}, w + "Read into 50 bytes");
    h.read(conn, 2048, w + "Read the first message", pump);
    h.read(conn, 2048, w + "Read the second message", pump);
    h.read(conn, 2048, w + "Read with nothing there", false, false);
    h.read(client, 2048, w + "Read the answer", pump);

    // Polling.
    const u64 poll = h.r("cellRudpPollCreate", {4}, w + "PollCreate");
    h.r("cellRudpPollControl", {poll, 1, static_cast<u64>(conn), 0x7}, w + "PollControl ADD");
    h.r("cellRudpPollControl", {poll, 1, static_cast<u64>(conn), 0x7}, w + "PollControl ADD again");
    h.r("cellRudpPollControl", {poll, 1, static_cast<u64>(client), 0x1}, w + "PollControl ADD the client");
    std::memset(b + kEvents, 0xEE, 32);
    h.r("cellRudpPollWait", {poll, kEvents, 4, 0}, w + "PollWait");
    h.o.line(w + "PollWait: the events", hex(b + kEvents, 16));
    h.r("cellRudpPollCancel", {poll}, w + "PollCancel");
    h.r("cellRudpPollDestroy", {poll}, w + "PollDestroy");

    // The end: the client terminates; the accepted context reads its end.
    h.r("cellRudpTerminate", {static_cast<u64>(client)}, w + "Terminate the client");
    h.within(5000ms, [&] {
        if (pump) h.side.call("cellRudpProcessEvents", {0});
        return h.status(conn, "", false) != 4;
    });
    h.status(conn, w + "the accepted context after the client's end");
    h.read(conn, 2048, w + "Read after the client's end", pump);
    h.o.line(w + "the events to the end", h.events(from));
    for (const s32 c : {conn, server}) h.r("cellRudpTerminate", {static_cast<u64>(c)}, w + "Terminate");
    h.r("cellRudpTerminate", {static_cast<u64>(server)}, w + "Terminate again");
    sc(h.t, 714, {static_cast<u64>(sa)});
    sc(h.t, 714, {static_cast<u64>(sb)});
}

Observed half(const Setup& s, bool sony_half)
{
    Run h(s, sony_half);
    u8* b = h.b;
    write_handler(b);
    sockaddr(b, kPeer, 7000);

    // Before Init.
    const std::vector<std::pair<const char*, std::vector<u64>>> all = {{"cellRudpEnd", {}},
        {"cellRudpEnableInternalIOThread", {0x4000, 1000}}, {"cellRudpSetEventHandler", {0, 0}},
        {"cellRudpSetMaxSegmentSize", {1400}}, {"cellRudpGetMaxSegmentSize", {kArgs}},
        {"cellRudpCreateContext", {kHandler, kRing, kArgs}}, {"cellRudpSetOption", {1, 8, kArgs, 4}},
        {"cellRudpGetOption", {1, 8, kArgs, 4}}, {"cellRudpGetContextStatus", {1, kStatus, 48}},
        {"cellRudpGetStatus", {kStatus, 400}}, {"cellRudpGetLocalInfo", {1, 0, 0, 0, 0, 0}},
        {"cellRudpGetRemoteInfo", {1, 0, 0, 0}}, {"cellRudpAccept", {1}}, {"cellRudpBind", {1, 24, 1, 0}},
        {"cellRudpListen", {1, 1}}, {"cellRudpInitiate", {1, kPeer, 16, 1}}, {"cellRudpActivate", {1, kPeer, 16}},
        {"cellRudpTerminate", {1}}, {"cellRudpRead", {1, kIn, 16, 0, 0}}, {"cellRudpWrite", {1, kOut, 16, 0}},
        {"cellRudpGetSizeReadable", {1}}, {"cellRudpGetSizeWritable", {1}}, {"cellRudpFlush", {1}},
        {"cellRudpPollCreate", {4}}, {"cellRudpPollDestroy", {1}}, {"cellRudpPollControl", {1, 1, 1, 1}},
        {"cellRudpPollWait", {1, kEvents, 4, 0}}, {"cellRudpPollCancel", {1}},
        {"cellRudpNetReceived", {24, kIn, 32, kPeer, 16}}, {"cellRudpProcessEvents", {0}}};
    for (const auto& [fn, args] : all) {
        trace(std::string("calling: ") + fn);
        std::optional<u64> v;
        switch (args.size()) { // initializer lists are fixed at compile time: the argument counts the library has
        case 0: v = h.side.call(fn, {}); break;
        case 1: v = h.side.call(fn, {args[0]}); break;
        case 2: v = h.side.call(fn, {args[0], args[1]}); break;
        case 3: v = h.side.call(fn, {args[0], args[1], args[2]}); break;
        case 4: v = h.side.call(fn, {args[0], args[1], args[2], args[3]}); break;
        case 5: v = h.side.call(fn, {args[0], args[1], args[2], args[3], args[4]}); break;
        default: v = h.side.call(fn, {args[0], args[1], args[2], args[3], args[4], args[5]}); break;
        }
        h.o.line(std::string(fn) + " before Init", v ? answer(*v) : std::string("(not exported)"));
    }

    // Init, the segment size.
    h.r("cellRudpInit", {0}, "Init");
    h.r("cellRudpInit", {0}, "Init again");
    std::memset(b + kArgs, 0xEE, 4);
    h.r("cellRudpGetMaxSegmentSize", {kArgs}, "GetMaxSegmentSize");
    h.o.line("GetMaxSegmentSize: the size", hex(b + kArgs, 4));
    const u16 mss = ps_read16(b, kArgs);
    for (const u32 v : {0u, 1u, 100u, 127u, 128u, 576u, 1500u, 9000u, 65507u, 65508u, 65535u}) {
        h.r("cellRudpSetMaxSegmentSize", {v}, std::format("SetMaxSegmentSize({})", v));
        std::memset(b + kArgs, 0xEE, 4);
        h.side.call("cellRudpGetMaxSegmentSize", {kArgs});
        h.o.line(std::format("SetMaxSegmentSize({}): the size after", v), hex(b + kArgs, 4));
    }
    h.side.call("cellRudpSetMaxSegmentSize", {mss});

    // Contexts and their options.
    h.r("cellRudpCreateContext", {kHandler, kRing, 0}, "CreateContext(NULL ID)");
    const s32 c = h.create("CreateContext");
    const s32 c2 = h.create("CreateContext again");
    h.create("CreateContext without a handler", 0);
    for (s32 opt = 0; opt <= 20; opt++) {
        h.option(c, opt, 4, std::format("GetOption({}) in 4 bytes", opt));
        h.option(c, opt, 8, std::format("GetOption({}) in 8 bytes", opt));
    }
    h.option(c, 2, 1, "GetOption(SNDBUF) in 1 byte");
    h.option(c, 8, 1, "GetOption(STREAM) in 1 byte");
    h.r("cellRudpGetOption", {static_cast<u64>(c), 8, kArgs, 0}, "GetOption(STREAM) in 0 bytes");
    h.r("cellRudpGetOption", {static_cast<u64>(c), 8, 0, 4}, "GetOption(STREAM) into NULL");
    h.r("cellRudpGetOption", {static_cast<u64>(c + 1000), 8, kArgs, 4}, "GetOption of no context");
    for (const auto& [opt, value] : {std::pair{1, 0u}, std::pair{1, 2000u}, std::pair{1, 100u}, std::pair{2, 0u},
             std::pair{2, 100u}, std::pair{2, 4096u}, std::pair{3, 1u}, std::pair{4, 1u}, std::pair{8, 2u},
             std::pair{9, 0u}, std::pair{14, 0u}, std::pair{18, 0u}, std::pair{20, 1u}}) {
        const std::string key = std::format("SetOption({}, {})", opt, value);
        h.set(c, opt, value, key);
        h.option(c, opt, 4, key + ", then");
    }
    h.status(c, "an idle context");
    std::memset(b + kStatus, 0xEE, 1024);
    h.r("cellRudpGetStatus", {kStatus, 1024}, "GetStatus");
    h.o.line("GetStatus: past the first 400 bytes", hex(b + kStatus + 400, 624));
    h.o.note("GetStatus: the library's status " + hex(b + kStatus, 400));
    h.r("cellRudpGetStatus", {kStatus, 0}, "GetStatus(size 0)");

    // Bind's checks, the local information.
    const s32 udp = h.socket(2, 7000), p2p = h.socket(6, 0, 77), tcp = h.socket(1, 0);
    h.r("cellRudpBind", {static_cast<u64>(c), 1000, 1, 0}, "Bind to no socket");
    h.r("cellRudpBind", {static_cast<u64>(c), static_cast<u64>(tcp), 1, 0}, "Bind to a TCP socket");
    h.r("cellRudpBind", {static_cast<u64>(c), static_cast<u64>(udp), 1, 2}, "Bind in mux mode 2");
    h.r("cellRudpBind", {static_cast<u64>(c), static_cast<u64>(udp), 1, 1}, "Bind RAW without an event handler");
    h.r("cellRudpBind", {static_cast<u64>(c), static_cast<u64>(udp), 0, 0}, "Bind at vport 0");
    std::memset(b + kArgs, 0xEE, 16);
    ps_write32(b, kArgs + 4, 16);
    h.r("cellRudpGetLocalInfo", {static_cast<u64>(c), kArgs, kAddr, kArgs + 4, kArgs + 8, kArgs + 10},
        "GetLocalInfo after Bind");
    h.o.line("GetLocalInfo after Bind: the socket is udp's, the vport, the mux mode",
        std::format("{} {} {}", static_cast<s32>(ps_read32(b, kArgs)) == udp ? "yes" : "no", ps_read16(b, kArgs + 8),
            ps_read8(b, kArgs + 10)));
    h.o.line("GetLocalInfo after Bind: the address", hex(b + kAddr, 16));
    h.r("cellRudpBind", {static_cast<u64>(c), static_cast<u64>(udp), 5, 0}, "Bind again");
    h.r("cellRudpBind", {static_cast<u64>(c2), static_cast<u64>(p2p), 5, 0}, "Bind to a UDP P2P socket");
    h.r("cellRudpInitiate", {static_cast<u64>(c2), kPeer, 16, 0}, "Initiate to vport 0");
    h.r("cellRudpInitiate", {static_cast<u64>(c2), kPeer, 8, 1}, "Initiate with 8 bytes of address");
    h.r("cellRudpRead", {static_cast<u64>(c2), kIn, 16, 1, 0}, "Read before a connection");
    h.r("cellRudpWrite", {static_cast<u64>(c2), kOut, 16, 0}, "Write before a connection");
    h.r("cellRudpAccept", {static_cast<u64>(c2)}, "Accept on a context not listening");
    for (const s32 x : {c, c2}) h.r("cellRudpTerminate", {static_cast<u64>(x)}, "Terminate");
    for (const s32 x : {udp, p2p, tcp}) sc(h.t, 714, {static_cast<u64>(x)});

    // A connection over the internal I/O thread, then (a new library) one driven by ProcessEvents.
    h.r("cellRudpEnableInternalIOThread", {0x4000, 1000}, "EnableInternalIOThread");
    h.r("cellRudpEnableInternalIOThread", {0x4000, 1000}, "EnableInternalIOThread again");
    connection(h, false, 7100);
    h.r("cellRudpEnd", {}, "End");
    h.r("cellRudpEnd", {}, "End again");
    h.r("cellRudpInit", {0}, "Init once more");
    connection(h, true, 7200);
    h.r("cellRudpEnd", {}, "the last End");
    return std::move(h.o);
}

} // namespace

TEST_CASE("cellRudp's comparison calls Sony's exports by their names; pspocket's library, firmware first",
    "[hle][rudp]")
{
    check_names("cellRudp", {"cellRudpInit", "cellRudpEnd", "cellRudpEnableInternalIOThread", "cellRudpSetEventHandler",
                                "cellRudpSetMaxSegmentSize", "cellRudpGetMaxSegmentSize", "cellRudpCreateContext",
                                "cellRudpSetOption", "cellRudpGetOption", "cellRudpGetContextStatus",
                                "cellRudpGetStatus", "cellRudpGetLocalInfo", "cellRudpGetRemoteInfo", "cellRudpAccept",
                                "cellRudpBind", "cellRudpListen", "cellRudpInitiate", "cellRudpActivate",
                                "cellRudpTerminate", "cellRudpRead", "cellRudpWrite", "cellRudpGetSizeReadable",
                                "cellRudpGetSizeWritable", "cellRudpFlush", "cellRudpPollCreate", "cellRudpPollDestroy",
                                "cellRudpPollControl", "cellRudpPollWait", "cellRudpPollCancel", "cellRudpNetReceived",
                                "cellRudpProcessEvents"});
}

TEST_CASE("cellRudp against Sony's librudp: answers, options, structures, a connection over each kind of I/O",
    "[.][hle][lle][compare][rudp]")
{
    const Setup s = setup();
    run(s, "cellRudp against Sony's librudp", [&](bool sony) { return half(s, sony); }, [](const Observed& o) {
        // pspocket's own half: test_rudp.cpp's answers.
        CHECK(o.value("cellRudpCreateContext before Init") == "0x80770001");
        CHECK(o.value("Init again") == "0x80770002");
        CHECK(o.value("GetMaxSegmentSize: the size") == "hex:0578eeee");
        CHECK(o.value("SetMaxSegmentSize(127)") == "0x80770004");
        CHECK(o.value("CreateContext again: the ID") == "first + 1");
        CHECK(o.value("GetOption(1) in 4 bytes: the value") == "hex:00000558eeeeeeee");
        CHECK(o.value("GetOption(12) in 4 bytes") == "0x80770005");
        CHECK(o.value("GetOption(SNDBUF) in 1 byte") == "0x8077001a");
        CHECK(o.value("Bind to a TCP socket") == "0x80770019");
        CHECK(o.value("Bind RAW without an event handler") == "0x80770022");
        CHECK(o.value("Bind again") == "0x8077001d");
        CHECK(o.value("Initiate to vport 0") == "0x80770015");
        CHECK(o.value("EnableInternalIOThread again") == "0x80770010");
        for (const auto& [w, accepted] : {std::pair{"the I/O thread: ", "first + 5"}, std::pair{"ProcessEvents: ",
                                                                                         "first + 8"}}) {
            INFO(w);
            const std::string k(w);
            CHECK(o.value(k + "Accept within 5 s") == accepted);
            CHECK(o.value(k + "Read the first message") == "0x64");
            CHECK(o.value(k + "Read into 50 bytes") == "0x8077001a");
            CHECK(o.value(k + "Read with nothing there") == "0x80770016");
            CHECK(o.value(k + "Read the answer") == "0x2bc");
            CHECK(o.value(k + "Read after the client's end") == "0x80770024");
            CHECK(o.value(k + "PollWait") == "0x1");
        }
        CHECK(o.lines.size() > 300);
    });
}
