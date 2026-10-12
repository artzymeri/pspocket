// cellRudp, pspocket's own (D-916): its answers as Sony's librudp gives them (test_rudp_compare.cpp compares every one
// with the module where the firmware is installed; these run everywhere): the argument checks and defaults, the wire
// format, a connection with the game as the network (the event handler's SEND, NetReceived) through a listener's cookie,
// data both ways with its read information, loss and retransmission, a reset and its end, packets for nobody, and a
// connection over the internal I/O thread with blocking contexts, its sockets through libnet. Synthetic: no game data.

#include "hle_test_util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"
#include "rudp.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
using namespace std::chrono_literals;
namespace as = pspocket::recomp::ppu::as;

namespace {

// RPCS3 cellRudp.h.
constexpr u32 NOT_INITIALIZED = 0x80770001, ALREADY_INITIALIZED = 0x80770002, INVALID_CONTEXT_ID = 0x80770003,
              INVALID_ARGUMENT = 0x80770004, INVALID_OPTION = 0x80770005, CONN_RESET = 0x80770009,
              THREAD_IN_USE = 0x80770010, NOT_ACCEPTABLE = 0x80770011, NOT_BOUND = 0x80770013,
              WOULDBLOCK = 0x80770016, INVALID_SOCKET = 0x80770019, BUFFER_TOO_SMALL = 0x8077001a,
              MSG_MALFORMED = 0x8077001b, ALREADY_BOUND = 0x8077001d, IN_PROGRESS = 0x80770021,
              NO_EVENT_HANDLER = 0x80770022, PAYLOAD_TOO_LARGE = 0x80770023, END_OF_DATA = 0x80770024;
constexpr u32 kEvClosed = 1, kEvEstablished = 2, kEvReadable = 5, kEvFlushed = 6, kEvAcceptable = 7;

constexpr u32 kCode = kData, kCtxOpd = kData + 0x400, kNetOpd = kData + 0x408;
constexpr u32 kArgs = kData + 0x800, kAddr = kData + 0x900, kPeer = kData + 0x940, kFrom = kData + 0x980;
constexpr u32 kCtxRing = kData + 0x1000, kCtxRecs = 256;
constexpr u32 kStatus = kData + 0x2000, kInfo = kData + 0x2400;
constexpr u32 kOut = kData + 0x10000, kIn = kData + 0x20000, kStage = kData + 0x30000;
constexpr u32 kNetRing = kData + 0x40000, kRec = 0x640, kRecs = 128, kRecData = 0x600;
constexpr u32 kNetParam = kData + 0x7F000, kNetPool = kData + 0x80000;

u64 code(u32 c) { return r3_of_code(c); }

// The context handler records {ctx, event, error}; the event handler {event, soc, datalen, addrlen, addr, data} and
// answers the length for SEND (as sent), 0 otherwise (test_rudp_compare.cpp's).
void write_handlers(u8* b)
{
    as::Assembler c(kCode);
    c.emit(as::lwz(7, 0, 6));
    c.emit(as::andi_(8, 7, kCtxRecs - 1));
    c.emit(as::slwi(8, 8, 4));
    c.emit(as::add(8, 8, 6));
    c.emit(as::stw(3, 16, 8));
    c.emit(as::stw(4, 20, 8));
    c.emit(as::stw(5, 24, 8));
    c.emit(as::lwsync());
    c.emit(as::addi(7, 7, 1));
    c.emit(as::stw(7, 0, 6));
    c.emit(as::li(3, 0));
    c.emit(as::blr());
    while (c.pc() < kCode + 0x100) c.emit(as::nop());
    c.emit(as::lwz(10, 0, 9));
    c.emit(as::andi_(11, 10, kRecs - 1));
    c.emit(as::mulli(11, 11, kRec));
    c.emit(as::add(11, 11, 9));
    c.emit(as::addi(11, 11, 16));
    c.emit(as::stw(3, 0, 11));
    c.emit(as::stw(4, 4, 11));
    c.emit(as::stw(6, 8, 11));
    c.emit(as::stw(8, 12, 11));
    const auto no_addr = c.new_label(), no_data = c.new_label(), short_data = c.new_label(), not_send = c.new_label();
    c.emit(as::cmplwi(0, 7, 0));
    c.beq(0, no_addr);
    c.emit(as::li(4, 16));
    c.emit(as::mtctr(4));
    c.emit(as::addi(7, 7, -1));
    c.emit(as::addi(12, 11, 15));
    const auto copy_addr = c.here();
    c.emit(as::lbzu(4, 1, 7));
    c.emit(as::stbu(4, 1, 12));
    c.bdnz(copy_addr);
    c.bind(no_addr);
    c.emit(as::cmplwi(0, 6, 0));
    c.beq(0, no_data);
    c.emit(as::mr(4, 6));
    c.emit(as::cmplwi(0, 4, kRecData));
    c.ble(0, short_data);
    c.emit(as::li(4, kRecData));
    c.bind(short_data);
    c.emit(as::mtctr(4));
    c.emit(as::addi(5, 5, -1));
    c.emit(as::addi(12, 11, 31));
    const auto copy_data = c.here();
    c.emit(as::lbzu(4, 1, 5));
    c.emit(as::stbu(4, 1, 12));
    c.bdnz(copy_data);
    c.bind(no_data);
    c.emit(as::lwsync());
    c.emit(as::addi(10, 10, 1));
    c.emit(as::stw(10, 0, 9));
    c.emit(as::cmpwi(0, 3, 1));
    c.bne(0, not_send);
    c.emit(as::mr(3, 6));
    c.emit(as::blr());
    c.bind(not_send);
    c.emit(as::li(3, 0));
    c.emit(as::blr());
    const std::vector<u8> bytes = c.finish();
    std::memcpy(b + kCode, bytes.data(), bytes.size());
    ps_write32(b, kCtxOpd, kCode);
    ps_write32(b, kCtxOpd + 4, 0);
    ps_write32(b, kNetOpd, kCode + 0x100);
    ps_write32(b, kNetOpd + 4, 0);
    ps_write32(b, kCtxRing, 0);
    ps_write32(b, kNetRing, 0);
}

s32 sc(GuestThread& g, u32 number, std::initializer_list<u64> args)
{
    u32 i = 3;
    for (const u64 a : args) g.ctx.gpr[i++] = a;
    g.ctx.gpr[11] = number;
    g.f.kernel->syscall(g.ctx);
    return static_cast<s32>(g.ctx.gpr[3]);
}

void sockaddr(u8* b, u32 at, u16 port)
{
    std::memset(b + at, 0, 16);
    ps_write8(b, at, 16);
    ps_write8(b, at + 1, 2);
    ps_write16(b, at + 2, port);
    ps_write32(b, at + 4, 0x7F000001);
}

struct Rec {
    s32 ctx = 0;
    u32 event = 0, error = 0;
};

struct Datagram {
    u32 event = 0;
    s32 soc = 0;
    u16 to = 0;
    std::vector<u8> bytes;
};

// A process with the handlers, one guest thread, the library's datagrams carried by the test.
struct Rudp : Fixture {
    GuestThread t{*this, 0, "game"};
    u8* b = base();
    std::map<u16, s32> by_port;
    std::map<s32, u16> port_of;
    u32 seen = 0;
    std::vector<Datagram> sent; // every datagram the event handler saw, in order
    std::function<bool(const Datagram&)> lose;

    Rudp()
    {
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
        ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
        write_handlers(b);
    }

    template <class... A>
    u64 call(std::string_view name, A... args)
    {
        return t.call(name, args...);
    }

    void network_up()
    {
        ps_write32(b, kNetParam, kNetPool);
        ps_write32(b, kNetParam + 4, 0x20000);
        ps_write32(b, kNetParam + 8, 0);
        REQUIRE(call("sys_net_initialize_network_ex", kNetParam) == 0);
    }

    s32 socket(u16 port)
    {
        const s32 s = sc(t, 713, {2, 2, 0});
        REQUIRE(s >= 0);
        sockaddr(b, kAddr, port);
        REQUIRE(sc(t, 701, {static_cast<u64>(s), kAddr, 16}) == 0);
        by_port[port] = s;
        port_of[s] = port;
        return s;
    }

    s32 create()
    {
        REQUIRE(call("cellRudpCreateContext", kCtxOpd, kCtxRing, kArgs) == 0);
        return static_cast<s32>(ps_read32(b, kArgs));
    }
    u64 set(s32 ctx, s32 option, u32 value)
    {
        ps_write32(b, kArgs + 0x40, value);
        return call("cellRudpSetOption", ctx, option, kArgs + 0x40, 4);
    }
    // An option's value, or its error with bits 32-35 set.
    u64 get(s32 ctx, s32 option)
    {
        ps_write32(b, kArgs + 0x48, 0xEEEEEEEE);
        const u64 r = call("cellRudpGetOption", ctx, option, kArgs + 0x48, 4);
        return r ? (r | 0xF00000000ull) : ps_read32(b, kArgs + 0x48);
    }
    u32 state(s32 ctx)
    {
        ps_write32(b, kStatus, 0xEEEEEEEE);
        call("cellRudpGetContextStatus", ctx, kStatus, 48);
        return ps_read32(b, kStatus);
    }
    u32 status_word(u32 offset)
    {
        REQUIRE(call("cellRudpGetStatus", kStatus, 0x128) == 0);
        return ps_read32(b, kStatus + offset);
    }

    std::vector<Rec> events()
    {
        const u32 n = load32_acquire(b, kCtxRing);
        std::vector<Rec> out;
        for (u32 i = n > kCtxRecs ? n - kCtxRecs : 0; i < n; i++) {
            const u32 at = kCtxRing + 16 + 16 * (i % kCtxRecs);
            out.push_back({static_cast<s32>(ps_read32(b, at)), ps_read32(b, at + 4), ps_read32(b, at + 8)});
        }
        return out;
    }
    bool heard(s32 ctx, u32 event, std::optional<u32> error = std::nullopt)
    {
        for (const Rec& r : events())
            if (r.ctx == ctx && r.event == event && (!error || r.error == *error)) return true;
        return false;
    }

    // The event handler's new records; SEND delivered to the socket at its destination (unless lost).
    void drain(bool deliver)
    {
        const u32 n = load32_acquire(b, kNetRing);
        for (; seen < n; seen++) {
            const u32 at = kNetRing + 16 + kRec * (seen % kRecs);
            Datagram d;
            d.event = ps_read32(b, at);
            d.soc = static_cast<s32>(ps_read32(b, at + 4));
            d.to = ps_read16(b, at + 18);
            const u32 len = std::min(ps_read32(b, at + 8), kRecData);
            d.bytes.assign(b + at + 32, b + at + 32 + len);
            sent.push_back(d);
            if (!deliver || d.event != 1 || (lose && lose(d))) continue;
            const auto dst = by_port.find(d.to);
            if (dst == by_port.end()) continue;
            std::memcpy(b + kStage, d.bytes.data(), d.bytes.size());
            sockaddr(b, kFrom, port_of[d.soc]);
            call("cellRudpNetReceived", dst->second, kStage, d.bytes.size(), kFrom, 16);
        }
    }

    // ProcessEvents and the network for `time`, or until `done`.
    bool pump(std::chrono::milliseconds time, const std::function<bool()>& done = {})
    {
        const auto until = std::chrono::steady_clock::now() + time;
        while (std::chrono::steady_clock::now() < until) {
            call("cellRudpProcessEvents", 0);
            drain(true);
            if (done && done()) return true;
            std::this_thread::sleep_for(1ms);
        }
        return done ? done() : true;
    }

    s32 write(s32 ctx, u32 size, u32 seed, u32 flags = 0)
    {
        for (u32 i = 0; i < size; i++) ps_write8(b, kOut + i, static_cast<u8>(seed + i * 7));
        return static_cast<s32>(call("cellRudpWrite", ctx, kOut, size, flags));
    }
    bool written(u32 at, u32 size, u32 seed)
    {
        for (u32 i = 0; i < size; i++)
            if (ps_read8(b, at + i) != static_cast<u8>(seed + i * 7)) return false;
        return true;
    }
};

// The packets of a datagram, read with rudp.hpp's header decoder.
std::vector<rudp::Header> packets(const std::vector<u8>& d)
{
    std::vector<rudp::Header> out;
    u32 at = 0;
    while (at + 2 <= d.size()) {
        const u16 prefix = static_cast<u16>(d[at] << 8 | d[at + 1]);
        const u32 vports = prefix >> 14, len = prefix & 0x7FF;
        if (!vports || at + len > d.size() || len < 2 * vports) break;
        rudp::Header h;
        if (rudp::decode(d.data() + at + 2 * vports, len - 2 * vports, h)) out.push_back(h);
        at += len;
    }
    return out;
}

} // namespace

TEST_CASE("cellRudp: every call before Init, the argument checks and the options' defaults as librudp has them",
    "[hle][rudp]")
{
    Rudp r;
    CHECK(r.call("cellRudpCreateContext", kCtxOpd, kCtxRing, kArgs) == code(NOT_INITIALIZED));
    CHECK(r.call("cellRudpEnd") == code(NOT_INITIALIZED));
    CHECK(r.call("cellRudpProcessEvents", 0) == code(NOT_INITIALIZED));
    REQUIRE(r.call("cellRudpInit", 0) == 0);
    CHECK(r.call("cellRudpInit", 0) == code(ALREADY_INITIALIZED));
    REQUIRE(r.call("cellRudpGetMaxSegmentSize", kArgs) == 0);
    CHECK(ps_read16(r.b, kArgs) == 0x582);
    CHECK(r.call("cellRudpSetMaxSegmentSize", 0x40) == 0); // any value while there is no context
    CHECK(r.call("cellRudpSetMaxSegmentSize", 0x582) == 0);

    const s32 c = r.create(), c2 = r.create();
    CHECK(c == 0); // IDs from 0
    CHECK(c2 == 1);
    CHECK(r.call("cellRudpSetMaxSegmentSize", 1000) == code(NOT_ACCEPTABLE));
    // Defaults: MAX_PAYLOAD 0x542, the buffers 64 KB, delivery and order critical, non-blocking, a 60 s connection
    // timeout, 30 ms aggregation, SACK on, 10 s cookies, a 20 s keep-alive timeout.
    const std::map<s32, u32> defaults = {{1, 0x542}, {2, 0x10000}, {3, 0x10000}, {4, 0}, {5, 1}, {6, 1}, {7, 1}, {8, 0},
        {9, 60000}, {10, 0}, {11, 30}, {12, 1}, {13, 10}, {14, 0}, {15, 0}, {16, 0}, {17, 0}, {18, 0}, {19, 20000}};
    for (const auto& [opt, v] : defaults) {
        INFO("option " << opt);
        CHECK(r.get(c, opt) == v);
    }
    CHECK(r.call("cellRudpGetOption", c, 0, kArgs, 4) == code(INVALID_OPTION));
    CHECK(r.call("cellRudpGetOption", c, 20, kArgs, 4) == code(INVALID_OPTION));
    CHECK(r.call("cellRudpGetOption", c, 8, kArgs, 8) == code(INVALID_ARGUMENT)); // 4 bytes only
    CHECK(r.call("cellRudpGetOption", c + 1000, 8, kArgs, 4) == code(INVALID_CONTEXT_ID));
    CHECK(r.set(c, 1, 0x543) == code(PAYLOAD_TOO_LARGE)); // past the segment size, less its 0x40 bytes
    CHECK(r.set(c, 4, 7) == 0);
    CHECK(r.get(c, 4) == 1); // booleans as 0 or 1
    CHECK(r.set(c, 14, 1) == code(INVALID_OPTION));
    CHECK(r.set(c, 0xFF, 1) == 0); // librudp's hidden option

    // The status structures: 1 to 48 and 1 to 0x128 bytes; GetStatus writes its counts of contexts and memory whatever
    // the size.
    CHECK(r.call("cellRudpGetContextStatus", c, kStatus, 49) == code(INVALID_ARGUMENT));
    CHECK(r.call("cellRudpGetStatus", kStatus, 0x129) == code(INVALID_ARGUMENT));
    std::memset(r.b + kStatus, 0xEE, 0x140);
    REQUIRE(r.call("cellRudpGetStatus", kStatus, 1) == 0);
    CHECK(ps_read8(r.b, kStatus + 1) == 0xEE);
    CHECK(ps_read32(r.b, kStatus + 0xF0) == 2); // the contexts

    // Bind: mux mode 1 or 2; Listen and Initiate want the event handler; Read and Write a connection.
    const s32 soc = r.socket(7000);
    CHECK(r.call("cellRudpGetLocalInfo", c, 0, 0, 0, 0, 0) == code(NOT_BOUND));
    CHECK(r.call("cellRudpBind", c, soc, 1, 0) == code(INVALID_ARGUMENT));
    CHECK(r.call("cellRudpBind", c, soc, 1, 3) == code(INVALID_ARGUMENT));
    REQUIRE(r.call("cellRudpBind", c, soc, 10, 2) == 0);
    CHECK(r.call("cellRudpBind", c, soc, 10, 2) == code(ALREADY_BOUND));
    CHECK(r.call("cellRudpListen", c, 4) == code(NO_EVENT_HANDLER));
    sockaddr(r.b, kPeer, 7001);
    CHECK(r.call("cellRudpInitiate", c, kPeer, 0x23, 1) == code(INVALID_ARGUMENT));
    CHECK(r.call("cellRudpInitiate", c, kPeer, 16, 1) == code(NO_EVENT_HANDLER));
    CHECK(r.call("cellRudpRead", c2, kIn, 16, 0, 0) == code(NOT_ACCEPTABLE));
    CHECK(r.call("cellRudpWrite", c2, kOut, 16, 0) == code(NOT_ACCEPTABLE));
    REQUIRE(r.call("cellRudpSetEventHandler", kNetOpd, kNetRing) == 0);
    CHECK(r.call("cellRudpListen", c, 0) == code(INVALID_ARGUMENT));
    REQUIRE(r.call("cellRudpListen", c, 4) == 0);
    CHECK(r.call("cellRudpListen", c, 4) == code(IN_PROGRESS));
    CHECK(r.call("cellRudpAccept", c, kCtxOpd, kCtxRing, kArgs, 0, 0, 0) == code(WOULDBLOCK));
    // Without the network GetLocalInfo's address stays zero: librudp asks libnet, which is not up (D-916 part 2).
    std::memset(r.b + kAddr, 0xEE, 16);
    ps_write32(r.b, kArgs + 4, 16);
    REQUIRE(r.call("cellRudpGetLocalInfo", c, kArgs, kAddr, kArgs + 4, kArgs + 8, kArgs + 10) == 0);
    CHECK(ps_read32(r.b, kArgs) == static_cast<u32>(soc));
    CHECK(ps_read32(r.b, kAddr) == 0);
    CHECK(ps_read16(r.b, kArgs + 8) == 10);
    CHECK(ps_read8(r.b, kArgs + 10) == 2);
    CHECK(r.call("cellRudpEnableInternalIOThread", 0x4000, 1000) == code(NOT_ACCEPTABLE)); // contexts exist
    CHECK(r.call("cellRudpNetReceived", -1, kIn, 16, kPeer, 16) == code(INVALID_SOCKET));
    CHECK(r.call("cellRudpNetReceived", soc, kIn, 0, kPeer, 16) == 0);
    REQUIRE(r.call("cellRudpEnd") == 0);
    CHECK(r.call("cellRudpEnd") == code(NOT_INITIALIZED));
}

TEST_CASE("cellRudp: a packet for nobody is passed over; a datagram is malformed only where it runs out (D-916)",
    "[hle][rudp]")
{
    Rudp r;
    REQUIRE(r.call("cellRudpInit", 0) == 0);
    REQUIRE(r.call("cellRudpSetEventHandler", kNetOpd, kNetRing) == 0);
    const s32 soc = r.socket(7400), x = r.create();
    REQUIRE(r.set(x, 13, 0) == 0);
    REQUIRE(r.call("cellRudpBind", x, soc, 10, 2) == 0);
    REQUIRE(r.call("cellRudpListen", x, 2) == 0);
    sockaddr(r.b, kFrom, 7401);
    const auto deliver = [&](std::vector<u8> d) {
        std::memcpy(r.b + kStage, d.data(), d.size());
        return r.call("cellRudpNetReceived", soc, kStage, d.size(), kFrom, 16);
    };
    // A SYN to vport 11 (nobody's), in mux mode 1 and without vports (the socket's contexts are mux mode 2): 0.
    CHECK(deliver({0xC0, 0x16, 0, 20, 0, 11, 0x80, 0x10, 0x12, 0x34, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0xA1, 0xB2,
              0xC3, 0xD4, 0x05, 0x82}) == 0);
    CHECK(deliver({0x80, 0x14, 0, 10, 0x80, 0x10, 0x12, 0x34, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0xA1, 0xB2, 0xC3, 0xD4,
              0x05, 0x82}) == 0);
    CHECK(deliver({0x40, 0x12, 0x80, 0x10, 0x12, 0x34, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0xA1, 0xB2, 0xC3, 0xD4, 0x05,
              0x82}) == 0);
    CHECK(deliver({0x00, 0x06, 0, 0, 0, 0}) == code(MSG_MALFORMED));                    // vport code 0
    CHECK(deliver({0xC0, 0x20, 0, 20, 0, 10, 0x80, 0x10, 0, 0}) == code(MSG_MALFORMED)); // longer than the datagram
    CHECK(deliver({0xC0, 0x20, 0, 20, 0, 11, 0x80, 0x10, 0, 0}) == code(MSG_MALFORMED)); // nobody's, and longer
    r.pump(50ms);
    CHECK(r.sent.empty()); // nothing answered
    REQUIRE(r.call("cellRudpEnd") == 0);
}

TEST_CASE("cellRudp: a connection with the game as the network: a listener's cookie, data both ways, a loss, the "
          "reset",
    "[hle][rudp]")
{
    Rudp r;
    REQUIRE(r.call("cellRudpInit", 0) == 0);
    REQUIRE(r.call("cellRudpSetEventHandler", kNetOpd, kNetRing) == 0);
    const s32 sa = r.socket(7100), sb = r.socket(7101);
    const s32 server = r.create(), client = r.create();
    REQUIRE(r.call("cellRudpBind", server, sa, 10, 2) == 0);
    REQUIRE(r.call("cellRudpBind", client, sb, 20, 2) == 0);
    REQUIRE(r.call("cellRudpListen", server, 4) == 0);
    sockaddr(r.b, kPeer, 7100);
    CHECK(r.call("cellRudpInitiate", client, kPeer, 16, 10) == 0);
    CHECK(r.state(client) == 2);
    REQUIRE(r.pump(2000ms, [&] { return r.heard(server, kEvAcceptable) && r.heard(client, kEvEstablished); }));

    // The handshake: SYN, the listener's RST with its 32-byte cookie (reason 2), the SYN again with it, SYN-ACK, ACK.
    std::vector<rudp::Header> hs;
    for (const Datagram& d : r.sent)
        for (const rudp::Header& h : packets(d.bytes)) hs.push_back(h);
    REQUIRE(hs.size() >= 4);
    CHECK(hs[0].type == rudp::kTypeSyn);
    CHECK(hs[0].version == 0x100);
    CHECK(hs[0].mss == 0x582);
    CHECK(hs[1].type == rudp::kTypeRst);
    CHECK(hs[1].code == 2);
    CHECK((hs[1].opts & rudp::kOptCookie));
    CHECK(hs[1].cookie_len == 32);
    CHECK(hs[2].type == rudp::kTypeSyn);
    CHECK(hs[2].cookie_len == 32);
    CHECK(hs[2].seq == hs[0].seq); // the same initial sequence number
    CHECK(hs[3].type == rudp::kTypeSyn);
    CHECK((hs[3].flags & rudp::kFlagAck));

    std::memset(r.b + kAddr, 0xEE, 0x28);
    ps_write32(r.b, kArgs + 4, 0x24);
    REQUIRE(r.call("cellRudpAccept", server, kCtxOpd, kCtxRing, kArgs, kAddr, kArgs + 4, kArgs + 8) == 0);
    const s32 child = static_cast<s32>(ps_read32(r.b, kArgs));
    CHECK(child == 2);
    CHECK(ps_read16(r.b, kAddr + 2) == 7101);
    CHECK(ps_read16(r.b, kAddr + 0x22) == 20); // the peer's vport after its address
    CHECK(r.state(child) == 4);
    CHECK(r.call("cellRudpAccept", server, kCtxOpd, kCtxRing, kArgs, 0, 0, 0) == code(WOULDBLOCK));

    // Messages: one per Read, the read information's size byte kept, a latency critical one counted when read.
    CHECK(r.write(client, 100, 1) == 100);
    CHECK(r.write(client, 64, 2, 0x08) == 64);
    CHECK(r.write(child, 700, 3) == 700);
    // READABLE comes with the next round of events, after the data.
    REQUIRE(r.pump(2000ms, [&] { return r.heard(child, kEvReadable); }));
    CHECK(r.call("cellRudpGetSizeReadable", child) == 100);
    CHECK(r.call("cellRudpRead", child, kIn, 50, 0, 0) == code(BUFFER_TOO_SMALL));
    std::memset(r.b + kInfo, 0xEE, 16);
    ps_write8(r.b, kInfo, 12);
    CHECK(r.call("cellRudpRead", child, kIn, 0x1000, 0, kInfo) == 100);
    CHECK(r.written(kIn, 100, 1));
    CHECK(ps_read8(r.b, kInfo) == 12);    // the size the game gave
    CHECK(ps_read8(r.b, kInfo + 5) == 0); // not latency critical
    CHECK(ps_read8(r.b, kInfo + 12) == 0xEE);
    const u32 latency_before = r.status_word(0x40);
    ps_write8(r.b, kInfo, 12);
    REQUIRE(r.pump(1000ms, [&] { return r.call("cellRudpGetSizeReadable", child) == 64; }));
    CHECK(r.call("cellRudpRead", child, kIn, 0x1000, 0, kInfo) == 64);
    CHECK(ps_read8(r.b, kInfo + 5) == 0x08);
    CHECK(r.status_word(0x40) == latency_before + 1);
    CHECK(r.call("cellRudpRead", child, kIn, 0x1000, 0, 0) == code(WOULDBLOCK));
    REQUIRE(r.pump(1000ms, [&] { return r.call("cellRudpGetSizeReadable", client) == 700; }));
    CHECK(r.call("cellRudpRead", client, kIn, 0x1000, 0, 0) == 700);
    CHECK(r.written(kIn, 700, 3));

    // A lost datagram is sent again (with its retransmission option), and read once.
    bool lost = false;
    r.lose = [&](const Datagram& d) {
        if (lost || d.to != 7100 || d.bytes.size() < 100) return false;
        for (const rudp::Header& h : packets(d.bytes))
            if (h.type == rudp::kTypeUsr) return lost = true;
        return false;
    };
    const usize before = r.sent.size();
    CHECK(r.write(client, 300, 4) == 300);
    REQUIRE(r.pump(5000ms, [&] { return r.call("cellRudpGetSizeReadable", child) == 300; }));
    CHECK(lost);
    bool retransmitted = false;
    for (usize i = before; i < r.sent.size(); i++)
        for (const rudp::Header& h : packets(r.sent[i].bytes))
            if (h.ext & rudp::kExtRetransmission) retransmitted = true;
    CHECK(retransmitted);
    CHECK(r.call("cellRudpRead", child, kIn, 0x1000, 0, 0) == 300);
    CHECK(r.written(kIn, 300, 4));
    CHECK(r.call("cellRudpRead", child, kIn, 0x1000, 0, 0) == code(WOULDBLOCK));
    REQUIRE(r.call("cellRudpGetContextStatus", client, kStatus, 48) == 0);
    CHECK(ps_read32(r.b, kStatus + 0x28) >= 1); // retransmissions

    // The client's Terminate: a RST (reason 0); the accepted context hears CLOSED with CONN_RESET and reads the end.
    r.lose = {};
    REQUIRE(r.call("cellRudpTerminate", client) == 0);
    // Terminated, it lingers for the library but is gone for the game.
    CHECK(r.call("cellRudpGetContextStatus", client, kStatus, 48) == code(INVALID_CONTEXT_ID));
    REQUIRE(r.pump(2000ms, [&] { return r.heard(child, kEvClosed, CONN_RESET); }));
    CHECK(r.state(child) == 5);
    CHECK(r.get(child, 14) == CONN_RESET);
    CHECK(r.get(child, 14) == 0); // read once
    CHECK(r.call("cellRudpRead", child, kIn, 0x1000, 0, 0) == code(END_OF_DATA));
    REQUIRE(r.call("cellRudpTerminate", child) == 0);
    REQUIRE(r.call("cellRudpTerminate", server) == 0);
    // A context that ended lingers until its timer lets it go.
    CHECK(r.pump(6000ms, [&] { return r.status_word(0xF0) == 0; }));
    REQUIRE(r.call("cellRudpEnd") == 0);
}

TEST_CASE("cellRudp: over the internal I/O thread with blocking contexts; its sockets go through libnet (D-916)",
    "[hle][rudp]")
{
    Rudp r;
    r.network_up();
    REQUIRE(r.call("cellRudpInit", 0) == 0);
    REQUIRE(r.call("cellRudpSetEventHandler", kNetOpd, kNetRing) == 0);
    CHECK(r.call("cellRudpEnableInternalIOThread", 0x4000, 0xC00) == code(INVALID_ARGUMENT));
    REQUIRE(r.call("cellRudpEnableInternalIOThread", 0x4000, 1000) == 0);
    CHECK(r.call("cellRudpEnableInternalIOThread", 0x4000, 1000) == code(THREAD_IN_USE));
    CHECK(r.call("cellRudpProcessEvents", 0) == code(THREAD_IN_USE));
    const s32 sa = r.socket(7500), sb = r.socket(7501);
    CHECK(r.call("cellRudpNetReceived", sa, kIn, 32, kPeer, 16) == code(THREAD_IN_USE));
    const s32 server = r.create(), client = r.create();
    REQUIRE(r.set(client, 7, 0) == 0); // blocking
    REQUIRE(r.set(client, 15, 150) == 0);
    REQUIRE(r.set(client, 17, 5000) == 0);
    REQUIRE(r.call("cellRudpBind", server, sa, 10, 2) == 0);
    REQUIRE(r.call("cellRudpBind", client, sb, 20, 2) == 0);
    REQUIRE(r.call("cellRudpListen", server, 4) == 0);
    sockaddr(r.b, kPeer, 7500);
    CHECK(r.call("cellRudpInitiate", client, kPeer, 16, 10) == 0); // returns established
    CHECK(r.state(client) == 4);
    // libnet answers getsockname now that the network is up.
    ps_write32(r.b, kArgs + 4, 16);
    REQUIRE(r.call("cellRudpGetLocalInfo", client, kArgs, kAddr, kArgs + 4, kArgs + 8, kArgs + 10) == 0);
    CHECK(ps_read16(r.b, kAddr + 2) == 7501);
    CHECK(ps_read32(r.b, kAddr + 4) == 0x7F000001);
    s32 child = -1;
    for (int i = 0; i < 2000 && child < 0; i++) {
        if (r.call("cellRudpAccept", server, kCtxOpd, kCtxRing, kArgs, 0, 0, 0) == 0)
            child = static_cast<s32>(ps_read32(r.b, kArgs));
        else std::this_thread::sleep_for(1ms);
    }
    REQUIRE(child >= 0);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(r.call("cellRudpRead", client, kIn, 64, 0, 0) == code(WOULDBLOCK)); // READ_TIMEOUT
    CHECK(std::chrono::steady_clock::now() - t0 >= 100ms);
    CHECK(r.call("cellRudpRead", client, kIn, 64, 1, 0) == code(WOULDBLOCK)); // DONTWAIT
    CHECK(r.write(child, 500, 5) == 500);
    CHECK(r.call("cellRudpRead", client, kIn, 0x1000, 0, 0) == 500); // waits for it
    CHECK(r.written(kIn, 500, 5));
    CHECK(r.write(client, 900, 6) == 900);
    CHECK(r.call("cellRudpFlush", client) == 0); // waits until the peer has it
    CHECK_FALSE(r.heard(client, kEvFlushed));    // a blocking context hears nothing
    for (int i = 0; i < 2000 && r.call("cellRudpGetSizeReadable", child) != 900; i++) std::this_thread::sleep_for(1ms);
    CHECK(r.call("cellRudpRead", child, kIn, 0x1000, 0, 0) == 900);
    // The event handler heard each sending (100) and receiving (101) of the I/O thread.
    r.drain(false);
    bool before_send = false, before_receive = false;
    for (const Datagram& d : r.sent) {
        before_send |= d.event == 100;
        before_receive |= d.event == 101;
    }
    CHECK(before_send);
    CHECK(before_receive);
    REQUIRE(r.call("cellRudpEnd") == 0);
    CHECK(r.call("sys_net_finalize_network") == 0);
}

TEST_CASE("cellRudp: a packet larger than the datagram leaves its start in it, its prefix 0, as librudp's (D-916)",
    "[hle][rudp]")
{
    // A segment size of 600 with the default payload of 0x542: a 1000-byte message does not fit a datagram. librudp
    // writes the packet's prefix place, its vport and header, then stops at the payload: those 8 bytes go out with the
    // datagram (its prefix 0), the message never does.
    Rudp r;
    REQUIRE(r.call("cellRudpInit", 0) == 0);
    REQUIRE(r.call("cellRudpSetEventHandler", kNetOpd, kNetRing) == 0);
    REQUIRE(r.call("cellRudpSetMaxSegmentSize", 600) == 0);
    const s32 sa = r.socket(8100), sb = r.socket(8101);
    const s32 x = r.create(), y = r.create();
    for (const s32 c : {x, y}) REQUIRE(r.set(c, 11, 0) == 0);
    REQUIRE(r.call("cellRudpBind", x, sa, 7, 1) == 0);
    REQUIRE(r.call("cellRudpBind", y, sb, 7, 1) == 0);
    sockaddr(r.b, kPeer, 8100);
    REQUIRE(r.call("cellRudpActivate", y, kPeer, 16) == 0);
    sockaddr(r.b, kPeer, 8101);
    REQUIRE(r.call("cellRudpInitiate", x, kPeer, 16, 7) == 0);
    REQUIRE(r.pump(2000ms, [&] { return r.heard(x, kEvEstablished) && r.heard(y, kEvEstablished); }));
    const usize before = r.sent.size();
    CHECK(r.write(x, 1000, 7) == 1000);
    r.pump(300ms);
    REQUIRE(r.sent.size() == before + 1);
    const std::vector<u8>& d = r.sent[before].bytes;
    REQUIRE(d.size() == 8);
    CHECK(d[0] == 0);
    CHECK(d[1] == 0);
    CHECK(d[2] == 0);
    CHECK(d[3] == 7);    // the vport
    CHECK(d[4] == 0x00); // a USR header without flags
    CHECK(r.call("cellRudpGetSizeReadable", y) == 0);
    REQUIRE(r.call("cellRudpEnd") == 0);
}
