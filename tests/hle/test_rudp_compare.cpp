// cellRudp against Sony's librudp (D-563, D-916): the same calls on the same inputs to pspocket's library and to Sony's
// module, compared as a game sees them:
// - every function's answer before Init, and after it each function's argument checks in Sony's order;
// - the maximum segment size, the contexts' IDs, every option's default, its checks and what each value reads back as;
// - the status structures, byte for byte (what each size writes, over a poison byte), the library's counters as each
//   step leaves them (its memory words too: the library's allocations as Sony's sizes them);
// - Bind, Listen, Initiate, Activate and Accept's checks and answers, the local and remote information;
// - connections driven by ProcessEvents with the game as the network: every datagram the library hands the event
//   handler (SEND) is described field by field (the wire format: vports, packet types, flags, windows, sequence and
//   acknowledgement numbers relative to each side's first, the handshake's parameters, the options, the payload) and
//   delivered to its peer with NetReceived, so a listener's cookies, the handshakes (client and server, P2P), data both
//   ways in each delivery mode, read information, flushing, polling, resets and the end show datagram by datagram;
// - malformed datagrams and datagrams to nobody (what NetReceived answers, what the library sends back);
// - a connection over the internal I/O thread (lv2's loopback sockets), with blocking contexts and the event handler's
//   hooks before each sending and receiving.
// Random values (initial sequence numbers, tokens, cookies), times (acknowledgement delays, time stamps, round trips)
// are masked and shown as notes. No game data; skipped without the firmware (compare_util.hpp).

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "compare_util.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;
using namespace std::chrono_literals;
namespace as = pspocket::recomp::ppu::as;

namespace {

constexpr u32 kCode = kData;                       // the handlers' code
constexpr u32 kCtxOpd = kData + 0x400, kNetOpd = kData + 0x408;
constexpr u32 kArgs = kData + 0x800;               // small outputs
constexpr u32 kAddr = kData + 0x900, kPeer = kData + 0x940, kFrom = kData + 0x980;
constexpr u32 kCtxRing = kData + 0x1000;           // context events: count, then {ctx, event, error, 0} from +16
constexpr u32 kCtxRecs = 256;
constexpr u32 kStatus = kData + 0x2000;            // status structures (0x200 bytes)
constexpr u32 kEvents = kData + 0x2400;            // poll events
constexpr u32 kOut = kData + 0x10000, kIn = kData + 0x20000; // 64 KB each
constexpr u32 kStage = kData + 0x30000;            // a datagram handed to NetReceived
constexpr u32 kNetRing = kData + 0x40000;          // event handler calls: count, then records from +16
constexpr u32 kRec = 0x640, kRecs = 128, kRecData = 0x600; // {event, soc, len, addrlen, addr[16], data[0x600]}
constexpr u32 kNetParam = kData + 0x7F000, kNetPool = kData + 0x80000; // libnet's parameters and pool (128 KB)


// The context handler (ctx_id, event_id, error_code, arg = ring) and the event handler (event_id, soc, data, datalen,
// addr, addrlen, arg = ring), guest code. The first records {ctx, event, error}; the second {event, soc, datalen,
// addrlen, addr (16 bytes), data (up to 0x600 bytes)} and answers the length for SEND (all sent), 0 for the rest.
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
    // The event handler.
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
    const std::vector<u8> code = c.finish();
    std::memcpy(b + kCode, code.data(), code.size());
    ps_write32(b, kCtxOpd, kCode);
    ps_write32(b, kCtxOpd + 4, 0);
    ps_write32(b, kNetOpd, kCode + 0x100);
    ps_write32(b, kNetOpd + 4, 0);
    ps_write32(b, kCtxRing, 0);
    ps_write32(b, kNetRing, 0);
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

void sockaddr(u8* b, u32 at, u16 port, u32 addr = 0x7F000001)
{
    std::memset(b + at, 0, 16);
    ps_write8(b, at, 16);
    ps_write8(b, at + 1, 2);
    ps_write16(b, at + 2, port);
    ps_write32(b, at + 4, addr);
}

u16 be16(const u8* p) { return static_cast<u16>(p[0] << 8 | p[1]); }
u32 be32(const u8* p) { return static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 | p[2] << 8 | p[3]; }

// The datagrams' fields as the comparison shows them: sequence numbers relative to the direction's initial one, tokens
// by order of appearance, cookies, delays and time stamps masked (the masked values go to a note).
struct Wire {
    using Dir = std::tuple<u16, u16, u16, u16>; // source port, source vport, destination port, destination vport
    std::map<Dir, u16> isn;
    std::map<u32, u32> tokens;
    std::string masked; // what describe() left out, for the note

    std::string token(u32 t)
    {
        const auto it = tokens.find(t);
        if (it != tokens.end()) return std::format("T{}", it->second);
        const u32 n = static_cast<u32>(tokens.size()) + 1;
        tokens.emplace(t, n);
        return std::format("T{}", n);
    }

    std::string describe(u16 src, u16 dst, const u8* p, u32 n)
    {
        masked.clear();
        std::string s;
        u32 at = 0;
        while (at + 2 <= n) {
            const u16 prefix = be16(p + at);
            const u32 code = prefix >> 14, plen = prefix & 0x7FF, mid = (prefix >> 11) & 7;
            s += std::format("{}[c{} l{}", s.empty() ? "" : " ", code, plen);
            if (mid) s += std::format(" m{}", mid);
            if (!code || plen < 2 * code || at + plen > n) {
                s += " malformed]";
                break;
            }
            const u32 end = at + plen;
            u32 q = at + 2;
            u16 sv = 0, dv = 0;
            if (code == 2) {
                sv = dv = be16(p + q);
                q += 2;
                s += std::format(" v{}", sv);
            } else if (code == 3) {
                sv = be16(p + q);
                dv = be16(p + q + 2);
                q += 4;
                s += std::format(" v{}>{}", sv, dv);
            }
            if (q + 4 > end) {
                s += " short]";
                at = end;
                continue;
            }
            const u8 b0 = p[q], win = p[q + 1];
            const u16 seq = be16(p + q + 2);
            q += 4;
            const u32 type = b0 >> 6, flags = b0 & 0x3F;
            static const char* const kTypes[] = {"USR", "PRB", "SYN", "RST"};
            s += std::format(" {} f{:02x} w{:02x}", kTypes[type], flags, win);
            const Dir key{src, sv, dst, dv}, back{dst, dv, src, sv};
            u16 ack = 0;
            if (flags & 0x20) {
                if (q + 2 > end) break;
                ack = be16(p + q);
                q += 2;
            }
            std::string isn_text;
            if (type == 2 && q + 12 <= end) {
                const auto it = isn.find(key);
                isn_text = it == isn.end() ? " isn" : it->second == seq ? " same-isn" : " new-isn";
                isn[key] = seq;
            }
            const auto base = isn.find(key);
            if (type == 3 && !seq) s += " s=0";
            else if (base != isn.end()) s += std::format(" s+{}", static_cast<u16>(seq - base->second));
            else if (!seq) s += " s=0";
            else {
                s += " s?";
                masked += std::format(" seq {:#x}", seq);
            }
            if (flags & 0x20) {
                const auto rb = isn.find(back);
                if (rb != isn.end()) s += std::format(" a+{}", static_cast<u16>(ack - rb->second));
                else {
                    s += " a?";
                    masked += std::format(" ack {:#x}", ack);
                }
            }
            if (flags & 0x04) {
                if (q + 2 > end) break;
                masked += std::format(" delay {}", be16(p + q));
                s += " delay";
                q += 2;
            }
            if (type == 2) {
                if (q + 12 > end) {
                    s += " short]";
                    at = end;
                    continue;
                }
                s += std::format(" p{:04x} ver{:04x}/{:04x} {} mss{}{}", be16(p + q), be16(p + q + 2),
                    be16(p + q + 4), token(be32(p + q + 6)), be16(p + q + 10), isn_text);
                q += 12;
            } else if (type == 3) {
                if (q + 6 > end) {
                    s += " short]";
                    at = end;
                    continue;
                }
                s += std::format(" code{:04x} {}", be16(p + q), token(be32(p + q + 2)));
                q += 6;
            }
            if (flags & 0x10) {
                u32 pads = 0;
                while (q < end) {
                    const u8 kind = p[q];
                    if (!kind) {
                        pads++;
                        q++;
                        continue;
                    }
                    if (q + 2 > end) {
                        s += " opt-short";
                        break;
                    }
                    const u8 k = kind & 0x7F, len = p[q + 1];
                    const u8* d = p + q + 2;
                    if (q + 2 + len > end) {
                        s += std::format(" opt{}-long", k);
                        q = end;
                        break;
                    }
                    if (k == 1) {
                        s += std::format(" cookie{}", len);
                        masked += " cookie " + hex(d, len).substr(4);
                    } else if (k == 2 && len == 2) {
                        s += std::format(" mss{}", be16(d));
                    } else if (k == 5) {
                        s += " sack(";
                        const auto rb = isn.find(back);
                        for (u32 i = 0; i + 4 <= len; i += 4) {
                            if (rb != isn.end())
                                s += std::format("{}{}-{}", i ? " " : "", static_cast<u16>(be16(d + i) - rb->second),
                                    static_cast<u16>(be16(d + i + 2) - rb->second));
                            else s += i ? " ?" : "?";
                        }
                        s += ")";
                    } else if (k == 10 && len == 4) {
                        s += std::format(" retx{}", d[0]);
                        masked += std::format(" retx-delay {}", be32(d) & 0xFFFFFF);
                    } else if (k == 11 && len == 4) {
                        s += " stamp";
                        masked += std::format(" stamp {}", be32(d));
                    } else {
                        s += std::format(" opt{}:{}", k, hex(d, len).substr(4));
                    }
                    q += 2 + len;
                    if (kind & 0x80) break;
                }
                if (pads) s += std::format(" pad{}", pads);
            }
            if (q < end) {
                const u32 len = end - q;
                s += std::format(" data{}", len);
                // The payload's first and last bytes (the tests' payloads are patterns).
                s += ":" + hex(p + q, std::min<u32>(len, 8)).substr(4);
                if (len > 8) s += ".." + hex(p + end - std::min<u32>(len - 8, 4), std::min<u32>(len - 8, 4)).substr(4);
            }
            s += "]";
            at = end;
        }
        if (at < n && at + 2 > n) s += std::format(" +{} bytes", n - at);
        return s.empty() ? "(empty)" : s;
    }
};

struct Run : Half<> {
    Wire wire;
    std::map<u16, s32> by_port; // the half's sockets
    std::map<s32, u16> port_of;
    u32 seen = 0;               // the event handler's records looked at
    u32 ctx_seen = 0;           // the context handler's
    u32 datagram = 0;           // datagrams described in the current step
    std::map<std::pair<u16, u16>, u32> per_direction; // the step's datagrams by (source port, destination port)
    std::map<std::pair<u16, u16>, std::vector<std::pair<std::string, std::string>>> step_lines; // their lines
    bool pump_events = true;    // ProcessEvents in the pump (no internal I/O thread)
    std::vector<u32> drop;      // datagrams of the step (by number) the network loses

    Run(const Setup& s, bool sony_half) : Half(s, sony_half, "cellRudp") {}

    // A UDP socket at 127.0.0.1:port.
    s32 socket(u16 port)
    {
        const s32 s = sc(t, 713, {2, 2, 0});
        sockaddr(b, kAddr, port);
        sc(t, 701, {static_cast<u64>(s), kAddr, 16});
        by_port[port] = s;
        port_of[s] = port;
        return s;
    }
    void close(s32 s)
    {
        sc(t, 714, {static_cast<u64>(s)});
        by_port.erase(port_of[s]);
        port_of.erase(s);
    }

    // CreateContext: its answer and the ID written.
    s32 create(const std::string& key, u32 handler = kCtxOpd)
    {
        ps_write32(b, kArgs, 0xEEEEEEEE);
        r("cellRudpCreateContext", {handler, kCtxRing, kArgs}, key);
        const s32 id = static_cast<s32>(ps_read32(b, kArgs));
        o.line(key + ": the ID", std::format("{}", id));
        return id;
    }

    u64 set(s32 ctx, s32 option, u32 value, const std::string& key, u32 len = 4)
    {
        ps_write32(b, kArgs + 0x40, value);
        ps_write32(b, kArgs + 0x44, 0);
        return r("cellRudpSetOption", {static_cast<u64>(ctx), static_cast<u64>(static_cast<u32>(option)), kArgs + 0x40,
                                          len},
            key);
    }

    // GetOption in `len` bytes: the answer and the bytes written (over 0xEE).
    void option(s32 ctx, s32 option, u32 len, const std::string& key)
    {
        std::memset(b + kArgs + 0x48, 0xEE, 8);
        r("cellRudpGetOption", {static_cast<u64>(ctx), static_cast<u64>(static_cast<u32>(option)), kArgs + 0x48, len},
            key);
        o.line(key + ": the value", hex(b + kArgs + 0x48, 8));
    }

    // CellRudpContextStatus (48 bytes, over 0xEE): the answer, the fields (the round trip a note).
    u32 ctx_status(s32 ctx, const std::string& key)
    {
        std::memset(b + kStatus, 0xEE, 64);
        const u64 a = side.call("cellRudpGetContextStatus", {static_cast<u64>(ctx), kStatus, 48}).value_or(~0ull);
        o.line(key + ": GetContextStatus", answer(a));
        o.line(key + ": state, parent, children", hex(b + kStatus, 12));
        o.line(key + ": lost, sent and received packets", hex(b + kStatus + 12, 12));
        o.line(key + ": sent and received bytes", hex(b + kStatus + 24, 16));
        o.line(key + ": retransmissions", hex(b + kStatus + 40, 4));
        o.line(key + ": past 48 bytes", hex(b + kStatus + 48, 16));
        o.note(key + std::format(": the round trip {} us", ps_read32(b, kStatus + 44)));
        return ps_read32(b, kStatus);
    }
    u32 state(s32 ctx)
    {
        ps_write32(b, kStatus + 0x100, 0xEEEEEEEE);
        side.call("cellRudpGetContextStatus", {static_cast<u64>(ctx), kStatus + 0x100, 4});
        return ps_read32(b, kStatus + 0x100);
    }

    // CellRudpStatus (0x128 bytes, over 0xEE), 16 bytes a line.
    void status(const std::string& key)
    {
        std::memset(b + kStatus, 0xEE, 0x140);
        r("cellRudpGetStatus", {kStatus, 0x128}, key + ": GetStatus");
        for (u32 at = 0; at < 0x128; at += 16)
            o.line(key + std::format(": status +{:#05x}", at), hex(b + kStatus + at, std::min<u32>(16, 0x128 - at)));
        o.line(key + ": past the status", hex(b + kStatus + 0x128, 16));
    }

    // The context handler's events since the last look: [ctx event error].
    std::string events(const std::string& key)
    {
        const u32 n = load32_acquire(b, kCtxRing);
        std::string s;
        for (; ctx_seen < n; ctx_seen++) {
            const u32 at = kCtxRing + 16 + 16 * (ctx_seen % kCtxRecs);
            s += std::format("{}[{} {} {:#x}]", s.empty() ? "" : " ", static_cast<s32>(ps_read32(b, at)),
                ps_read32(b, at + 4), ps_read32(b, at + 8));
        }
        s = s.empty() ? "(none)" : s;
        o.line(key + ": the context events", s);
        return s;
    }

    // The event handler's new records: SEND (and the I/O thread's sendings, 100) described and, for SEND, delivered
    // to the socket at their destination with NetReceived (unless the step drops them).
    void drain(const std::string& step)
    {
        const u32 n = load32_acquire(b, kNetRing);
        for (; seen < n; seen++) {
            const u32 at = kNetRing + 16 + kRec * (seen % kRecs);
            const u32 ev = ps_read32(b, at), soc = ps_read32(b, at + 4), len = ps_read32(b, at + 8),
                      addrlen = ps_read32(b, at + 12);
            const u16 dport = ps_read16(b, at + 18);
            if (ev == 101) continue; // the I/O thread's receivings: the datagrams of its sendings
            const auto sp = port_of.find(static_cast<s32>(soc));
            const u16 sport = sp == port_of.end() ? 0 : sp->second;
            const u32 copied = std::min(len, kRecData);
            std::string line = std::format("event {} from {} to {} ({} bytes of address, family {}) {} bytes: ", ev,
                sport ? std::format("port {}", sport) : std::string("another socket"),
                dport ? std::format("port {}", dport) : std::string("port 0"), addrlen, ps_read8(b, at + 17), len);
            line += wire.describe(sport, dport, b + at + 32, copied);
            const u32 number = datagram++;
            const u32 nth = per_direction[{sport, dport}]++;
            const bool lost = std::find(drop.begin(), drop.end(), number) != drop.end();
            if (lost) line += " (lost)";
            if (ev == 1 && !lost) {
                const auto dst = by_port.find(dport);
                if (dst == by_port.end()) {
                    line += " (nobody there)";
                } else {
                    std::memcpy(b + kStage, b + at + 32, copied);
                    sockaddr(b, kFrom, sport);
                    const u64 a = side.call("cellRudpNetReceived",
                                          {static_cast<u64>(dst->second), kStage, copied, kFrom, 16})
                                      .value_or(~0ull);
                    if (a) line += " NetReceived " + answer(a);
                }
            }
            // Numbered in each direction: the two ends' timers interleave their datagrams as the times fall, which the
            // halves need not share; each direction's sequence is compared, the order of all of them is a note.
            step_lines[{sport, dport}].emplace_back(step + std::format(": from port {} to port {}: datagram {}", sport,
                                                        dport, nth),
                line);
            o.note(step + std::format(": datagram {} is from port {} to port {}", number, sport, dport));
            if (!wire.masked.empty()) o.note(step + std::format(": datagram {}:{}", number, wire.masked));
            o.note(step + std::format(": datagram {} bytes ", number) + hex(b + at + 32, copied).substr(4));
        }
    }

    // Runs the library for `time` (ProcessEvents(0) every millisecond, or the I/O thread on its own), the network
    // in between; the step's datagrams are numbered from 0.
    void pump(const std::string& step, std::chrono::milliseconds time)
    {
        start_step();
        const auto until = std::chrono::steady_clock::now() + time;
        do {
            if (pump_events) side.call("cellRudpProcessEvents", {0});
            drain(step);
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < until);
        finish_step(step);
    }

    // Runs the library until every context has gone (a connection that ended lingers until its timer lets it go),
    // up to 6 s, the network as pump's: whether they went (how long it took is a note, a time the halves need not
    // share: the moment a context goes changes what the library holds when, so each part ends with this).
    void settle(const std::string& step)
    {
        start_step();
        const auto t0 = std::chrono::steady_clock::now();
        bool gone = false;
        while (std::chrono::steady_clock::now() - t0 < 6s) {
            if (pump_events) side.call("cellRudpProcessEvents", {0});
            drain(step);
            ps_write32(b, kStatus + 0x200 + 0xF0, 0xEEEEEEEE);
            side.call("cellRudpGetStatus", {kStatus + 0x200, 0x128});
            if (!ps_read32(b, kStatus + 0x200 + 0xF0)) {
                gone = true;
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        // The timers of what went stay until they come due (each then freed): 300 ms more.
        const auto until = std::chrono::steady_clock::now() + 300ms;
        while (std::chrono::steady_clock::now() < until) {
            if (pump_events) side.call("cellRudpProcessEvents", {0});
            drain(step);
            std::this_thread::sleep_for(1ms);
        }
        finish_step(step);
        o.line(step + ": every context gone within 6 s", gone ? "yes" : "no");
        o.note(step + std::format(": the contexts went in {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                       std::chrono::steady_clock::now() - t0)
                                                                       .count()));
    }

    void start_step()
    {
        datagram = 0;
        per_direction.clear();
        step_lines.clear();
    }
    void finish_step(const std::string& step)
    {
        drain(step);
        for (auto& [dir, lines] : step_lines)
            for (auto& [key, value] : lines) o.line(std::move(key), std::move(value));
        step_lines.clear();
        std::string counts;
        for (const auto& [dir, n] : per_direction) counts += std::format(" {}>{}: {}", dir.first, dir.second, n);
        o.line(step + ": datagrams", counts.empty() ? std::string("none") : counts.substr(1));
        drop.clear();
    }

    s32 write(s32 ctx, u32 size, u32 seed, const std::string& key, u32 flags = 0)
    {
        for (u32 i = 0; i < size; i++) ps_write8(b, kOut + i, static_cast<u8>(seed + i * 7));
        return static_cast<s32>(r("cellRudpWrite", {static_cast<u64>(ctx), kOut, size, flags}, key));
    }

    // Read into `len` bytes with 12 bytes of read information: the answer, the bytes, the information (its sequence
    // number and time stamp a note).
    s32 read(s32 ctx, u32 len, const std::string& key, u32 flags = 0, u32 info_size = 12)
    {
        std::memset(b + kIn, 0xEE, 16);
        ps_write8(b, kIn, static_cast<u8>(info_size));
        std::memset(b + kIn + 0x100, 0xEE, std::min<u32>(len + 16, 0x8000));
        const u64 a = r("cellRudpRead", {static_cast<u64>(ctx), kIn + 0x100, len, flags, kIn}, key);
        const s32 n = static_cast<s32>(a);
        if (n > 0) {
            o.line(key + ": the data", hex(b + kIn + 0x100, static_cast<usize>(std::min(n, 32))));
            if (n > 32) o.line(key + ": the data's end", hex(b + kIn + 0x100 + n - 16, 16));
            o.line(key + ": past the data", hex(b + kIn + 0x100 + n, 4));
        }
        o.line(key + ": the read information", hex(b + kIn, 6) + " " + hex(b + kIn + 12, 4));
        o.note(key + ": the read information's sequence number and time stamp " + hex(b + kIn + 6, 6));
        return n;
    }

    void sizes(s32 ctx, const std::string& key)
    {
        r("cellRudpGetSizeReadable", {static_cast<u64>(ctx)}, key + ": GetSizeReadable");
        r("cellRudpGetSizeWritable", {static_cast<u64>(ctx)}, key + ": GetSizeWritable");
    }

    // Accept with every output: the answer, the ID, the address (36 bytes over 0xEE), its length, the vport.
    s32 accept(s32 listener, const std::string& key, u32 addrlen = 0x24)
    {
        std::memset(b + kAddr, 0xEE, 0x30);
        ps_write32(b, kArgs, 0xEEEEEEEE);
        ps_write32(b, kArgs + 4, addrlen);
        ps_write32(b, kArgs + 8, 0xEEEEEEEE);
        const u64 a = r("cellRudpAccept", {static_cast<u64>(listener), kCtxOpd, kCtxRing, kArgs, kAddr, kArgs + 4,
                                              kArgs + 8},
            key);
        o.line(key + ": the ID, the address's length, the vport", hex(b + kArgs, 12));
        o.line(key + ": the address", hex(b + kAddr, 0x28));
        return static_cast<s32>(a) == 0 ? static_cast<s32>(ps_read32(b, kArgs)) : -1;
    }

    void remote(s32 ctx, const std::string& key, u32 addrlen = 0x22)
    {
        std::memset(b + kAddr, 0xEE, 0x30);
        ps_write32(b, kArgs, addrlen);
        ps_write32(b, kArgs + 4, 0xEEEEEEEE);
        r("cellRudpGetRemoteInfo", {static_cast<u64>(ctx), kAddr, kArgs, kArgs + 4}, key + ": GetRemoteInfo");
        o.line(key + ": GetRemoteInfo: the address", hex(b + kAddr, 0x24));
        o.line(key + ": GetRemoteInfo: its length, the vport", hex(b + kArgs, 8));
    }

    void local(s32 ctx, const std::string& key, u32 addrlen = 0x22)
    {
        std::memset(b + kAddr, 0xEE, 0x30);
        std::memset(b + kArgs, 0xEE, 16);
        ps_write32(b, kArgs + 4, addrlen);
        r("cellRudpGetLocalInfo", {static_cast<u64>(ctx), kArgs, kAddr, kArgs + 4, kArgs + 8, kArgs + 10},
            key + ": GetLocalInfo");
        const s32 soc = static_cast<s32>(ps_read32(b, kArgs));
        const auto p = port_of.find(soc);
        o.line(key + ": GetLocalInfo: the socket", p != port_of.end() ? std::format("port {}'s", p->second)
                                                                       : hex(b + kArgs, 4));
        o.line(key + ": GetLocalInfo: the address", hex(b + kAddr, 0x24));
        o.line(key + ": GetLocalInfo: its length, the vport, the mux mode", hex(b + kArgs + 4, 8));
    }
};

// Each function before Init.
void before_init(Run& h)
{
    const std::vector<std::pair<const char*, std::vector<u64>>> all = {{"cellRudpEnd", {}},
        {"cellRudpEnableInternalIOThread", {0x4000, 1000}}, {"cellRudpSetEventHandler", {0, 0}},
        {"cellRudpSetMaxSegmentSize", {1400}}, {"cellRudpGetMaxSegmentSize", {kArgs}},
        {"cellRudpCreateContext", {kCtxOpd, kCtxRing, kArgs}}, {"cellRudpSetOption", {0, 8, kArgs, 4}},
        {"cellRudpGetOption", {0, 8, kArgs, 4}}, {"cellRudpGetContextStatus", {0, kStatus, 48}},
        {"cellRudpGetStatus", {kStatus, 0x128}}, {"cellRudpGetLocalInfo", {0, 0, 0, 0, 0, 0}},
        {"cellRudpGetRemoteInfo", {0, 0, 0, 0}}, {"cellRudpAccept", {0, 0, 0, kArgs, 0, 0, 0}},
        {"cellRudpBind", {0, 24, 1, 2}}, {"cellRudpListen", {0, 1}}, {"cellRudpInitiate", {0, kPeer, 16, 1}},
        {"cellRudpActivate", {0, kPeer, 16}}, {"cellRudpTerminate", {0}}, {"cellRudpRead", {0, kIn, 16, 0, 0}},
        {"cellRudpWrite", {0, kOut, 16, 0}}, {"cellRudpGetSizeReadable", {0}}, {"cellRudpGetSizeWritable", {0}},
        {"cellRudpFlush", {0}}, {"cellRudpPollCreate", {4}}, {"cellRudpPollDestroy", {0}},
        {"cellRudpPollControl", {0, 1, 0, 1}}, {"cellRudpPollWait", {0, kEvents, 4, 0}}, {"cellRudpPollCancel", {0}},
        {"cellRudpNetReceived", {24, kIn, 32, kPeer, 16}}, {"cellRudpProcessEvents", {0}}};
    for (const auto& [fn, a] : all) {
        trace(std::string("calling: ") + fn);
        std::optional<u64> v;
        switch (a.size()) { // initializer lists are fixed at compile time: the argument counts the library has
        case 0: v = h.side.call(fn, {}); break;
        case 1: v = h.side.call(fn, {a[0]}); break;
        case 2: v = h.side.call(fn, {a[0], a[1]}); break;
        case 3: v = h.side.call(fn, {a[0], a[1], a[2]}); break;
        case 4: v = h.side.call(fn, {a[0], a[1], a[2], a[3]}); break;
        case 5: v = h.side.call(fn, {a[0], a[1], a[2], a[3], a[4]}); break;
        case 6: v = h.side.call(fn, {a[0], a[1], a[2], a[3], a[4], a[5]}); break;
        default: v = h.side.call(fn, {a[0], a[1], a[2], a[3], a[4], a[5], a[6]}); break;
        }
        h.o.line(std::string(fn) + " before Init", v ? answer(*v) : std::string("(not exported)"));
    }
}

// The segment size, the contexts, their options and status structures, the library's status.
void basics(Run& h)
{
    u8* b = h.b;
    h.r("cellRudpInit", {0}, "Init");
    h.r("cellRudpInit", {0}, "Init again");
    h.status("after Init");
    h.r("cellRudpProcessEvents", {0}, "ProcessEvents(0) with nothing to do");
    h.r("cellRudpProcessEvents", {2000}, "ProcessEvents(2 ms) with nothing to do");
    h.r("cellRudpGetMaxSegmentSize", {0}, "GetMaxSegmentSize(NULL)");
    std::memset(b + kArgs, 0xEE, 4);
    h.r("cellRudpGetMaxSegmentSize", {kArgs}, "GetMaxSegmentSize");
    h.o.line("GetMaxSegmentSize: the size", hex(b + kArgs, 4));
    const u16 mss = ps_read16(b, kArgs);
    for (const u32 v : {0u, 1u, 100u, 0x40u, 0x41u, 576u, 1500u, 9000u, 65535u, 0x12345u}) {
        h.r("cellRudpSetMaxSegmentSize", {v}, std::format("SetMaxSegmentSize({:#x})", v));
        std::memset(b + kArgs, 0xEE, 4);
        h.side.call("cellRudpGetMaxSegmentSize", {kArgs});
        h.o.line(std::format("SetMaxSegmentSize({:#x}): the size after", v), hex(b + kArgs, 4));
    }
    h.side.call("cellRudpSetMaxSegmentSize", {mss});
    h.r("cellRudpSetEventHandler", {0, 0}, "SetEventHandler(NULL)");

    // Contexts.
    h.r("cellRudpCreateContext", {kCtxOpd, kCtxRing, 0}, "CreateContext(NULL ID)");
    const s32 c = h.create("CreateContext");
    const s32 c2 = h.create("CreateContext again");
    const s32 c3 = h.create("CreateContext without a handler", 0);
    h.status("with three contexts");
    for (s32 opt = 0; opt <= 20; opt++) {
        h.option(c, opt, 4, std::format("GetOption({}) in 4 bytes", opt));
        h.option(c, opt, 8, std::format("GetOption({}) in 8 bytes", opt));
    }
    h.option(c, 0xFF, 4, "GetOption(0xff)");
    h.option(c, -1, 4, "GetOption(-1)");
    h.option(c, 2, 1, "GetOption(SNDBUF) in 1 byte");
    h.option(c, 2, 0, "GetOption(SNDBUF) in 0 bytes");
    h.option(c, 30, 0, "GetOption(30) in 0 bytes");
    h.r("cellRudpGetOption", {static_cast<u64>(c), 8, 0, 4}, "GetOption(STREAM) into NULL");
    h.r("cellRudpGetOption", {static_cast<u64>(c + 1000), 8, kArgs, 4}, "GetOption of no context");
    h.r("cellRudpGetOption", {static_cast<u64>(-1), 8, kArgs, 4}, "GetOption of context -1");
    h.r("cellRudpGetOption", {static_cast<u64>(c + 1000), 30, kArgs, 0}, "GetOption(30) of no context");

    // Each option's checks and what each value reads back as (on its own context: c2 and c3).
    for (const auto& [opt, value] : std::vector<std::pair<s32, u32>>{{1, 0}, {1, 1}, {1, 0x542}, {1, 0x543}, {1, 0x1000},
             {1, 0xFFFFFFC0}, {1, 0x200}, {2, 0}, {2, 0x23F}, {2, 0x240}, {2, 0x100000}, {3, 0x241}, {3, 0x10},
             {1, 0x400}, {4, 2}, {4, 0}, {5, 0}, {5, 7}, {6, 0}, {7, 0}, {7, 5}, {7, 1}, {8, 1}, {8, 0}, {9, 0},
             {9, 1}, {10, 5000}, {11, 0}, {11, 2000}, {12, 0}, {13, 0}, {13, 100}, {14, 1}, {15, 100}, {16, 200},
             {17, 300}, {18, 1000}, {18, 0}, {19, 0}, {20, 1}, {0, 1}, {-1, 1}, {0xFF, 1}, {0xFE, 1}}) {
        const std::string key = std::format("SetOption({}, {:#x})", opt, value);
        h.set(c2, opt, value, key);
        if (opt > 0 && opt < 20) h.option(c2, opt, 4, key + ", then");
    }
    h.set(c3, 1, 0x100, "SetOption(1) in 8 bytes", 8);
    h.set(c3, 1, 0x100, "SetOption(1) in 0 bytes", 0);
    h.set(c3, 4, 1, "SetOption(4) in 2 bytes", 2);
    h.set(c3, 15, 1, "SetOption(15) in 8 bytes", 8);
    h.set(c3, 30, 1, "SetOption(30) in 0 bytes", 0);
    h.r("cellRudpSetOption", {static_cast<u64>(c3), 4, 0, 4}, "SetOption(4) from NULL");
    h.r("cellRudpSetOption", {static_cast<u64>(c3 + 1000), 4, kArgs, 4}, "SetOption of no context");
    h.option(c3, 1, 4, "SetOption in bad sizes: MAX_PAYLOAD after");

    // The status structures in each size.
    for (const u32 size : {0u, 1u, 4u, 47u, 48u, 49u}) {
        std::memset(b + kStatus, 0xEE, 64);
        h.r("cellRudpGetContextStatus", {static_cast<u64>(c), kStatus, size}, std::format("GetContextStatus({})", size));
        h.o.line(std::format("GetContextStatus({}): the bytes", size), hex(b + kStatus, 52));
    }
    h.r("cellRudpGetContextStatus", {static_cast<u64>(c), 0, 48}, "GetContextStatus into NULL");
    h.r("cellRudpGetContextStatus", {static_cast<u64>(c + 1000), kStatus, 48}, "GetContextStatus of no context");
    for (const u32 size : {0u, 1u, 0xF0u, 0x127u, 0x128u, 0x129u}) {
        std::memset(b + kStatus, 0xEE, 0x140);
        h.r("cellRudpGetStatus", {kStatus, size}, std::format("GetStatus({:#x})", size));
        for (u32 at = 0; at < 0x130; at += 16)
            h.o.line(std::format("GetStatus({:#x}): +{:#05x}", size, at), hex(b + kStatus + at, 16));
    }
    h.r("cellRudpGetStatus", {0, 0x128}, "GetStatus into NULL");

    // Bind's checks, the local information, Listen, Initiate, Activate, Accept, Read and Write before a connection.
    const s32 udp = h.socket(7000), udp2 = h.socket(7001);
    h.r("cellRudpGetLocalInfo", {static_cast<u64>(c), 0, 0, 0, 0, 0}, "GetLocalInfo before Bind");
    h.r("cellRudpGetRemoteInfo", {static_cast<u64>(c), 0, 0, 0}, "GetRemoteInfo before Bind");
    for (const u32 mux : {0u, 3u, 0x101u})
        h.r("cellRudpBind", {static_cast<u64>(c), static_cast<u64>(udp), 1, mux}, std::format("Bind in mux mode {:#x}", mux));
    h.r("cellRudpBind", {static_cast<u64>(c), static_cast<u64>(-1), 1, 2}, "Bind to socket -1");
    h.r("cellRudpBind", {static_cast<u64>(c + 1000), static_cast<u64>(udp), 1, 2}, "Bind of no context");
    h.r("cellRudpListen", {static_cast<u64>(c), 4}, "Listen before Bind, without an event handler");
    h.r("cellRudpBind", {static_cast<u64>(c), static_cast<u64>(udp), 0, 2}, "Bind at vport 0");
    h.local(c, "after Bind at vport 0");
    h.r("cellRudpBind", {static_cast<u64>(c2), static_cast<u64>(udp), 0, 2}, "Bind another at vport 0");
    h.local(c2, "the other after Bind at vport 0");
    h.r("cellRudpBind", {static_cast<u64>(c), static_cast<u64>(udp), 5, 2}, "Bind again");
    h.local(c, "after Bind again");
    h.r("cellRudpBind", {static_cast<u64>(c3), static_cast<u64>(udp), 5, 2}, "Bind a third at the same vport");
    h.r("cellRudpBind", {static_cast<u64>(c3), static_cast<u64>(udp), 5, 1}, "Bind a third at the same vport, P2P");
    h.local(c3, "the third");
    h.local(c3, "the third, the address in 8 bytes", 8);
    h.local(c3, "the third, the address in 0 bytes", 0);
    h.r("cellRudpGetLocalInfo", {static_cast<u64>(c3), 0, 0, 0, 0, 0}, "GetLocalInfo with no outputs");
    h.r("cellRudpListen", {static_cast<u64>(c), 4}, "Listen without an event handler");
    h.r("cellRudpInitiate", {static_cast<u64>(c), kPeer, 16, 1}, "Initiate without an event handler");
    h.r("cellRudpActivate", {static_cast<u64>(c), kPeer, 16}, "Activate without an event handler");
    h.r("cellRudpSetEventHandler", {kNetOpd, kNetRing}, "SetEventHandler");
    h.r("cellRudpListen", {static_cast<u64>(c), 0}, "Listen(backlog 0)");
    h.r("cellRudpListen", {static_cast<u64>(c), static_cast<u64>(-1)}, "Listen(backlog -1)");
    h.r("cellRudpListen", {static_cast<u64>(c3), 4}, "Listen on a P2P context");
    h.r("cellRudpListen", {static_cast<u64>(c + 1000), 4}, "Listen of no context");
    h.r("cellRudpAccept", {static_cast<u64>(c), kCtxOpd, kCtxRing, kArgs, 0, 0, 0}, "Accept on a context not listening");
    h.r("cellRudpAccept", {static_cast<u64>(c), kCtxOpd, kCtxRing, 0, 0, 0, 0}, "Accept into NULL");
    h.r("cellRudpListen", {static_cast<u64>(c), 1000}, "Listen(backlog 1000)");
    h.ctx_status(c, "a listener");
    h.r("cellRudpListen", {static_cast<u64>(c), 4}, "Listen again");
    h.r("cellRudpBind", {static_cast<u64>(c), static_cast<u64>(udp), 6, 2}, "Bind a listener");
    h.accept(c, "Accept with nothing there");
    h.r("cellRudpInitiate", {static_cast<u64>(c), kPeer, 16, 1}, "Initiate on a listener");
    h.r("cellRudpRead", {static_cast<u64>(c), kIn, 16, 0, 0}, "Read on a listener");
    h.r("cellRudpWrite", {static_cast<u64>(c), kOut, 16, 0}, "Write on a listener");
    h.r("cellRudpFlush", {static_cast<u64>(c)}, "Flush on a listener");
    h.sizes(c, "a listener");
    h.remote(c, "a listener");
    sockaddr(b, kPeer, 7001);
    h.r("cellRudpInitiate", {static_cast<u64>(c2), 0, 16, 1}, "Initiate to NULL");
    h.r("cellRudpInitiate", {static_cast<u64>(c2), kPeer, 0x23, 1}, "Initiate with 35 bytes of address");
    h.r("cellRudpActivate", {static_cast<u64>(c2), kPeer, 0x23}, "Activate with 35 bytes of address");
    h.r("cellRudpRead", {static_cast<u64>(c2), kIn, 16, 0, 0}, "Read before a connection");
    h.r("cellRudpRead", {static_cast<u64>(c2), 0, 16, 0, 0}, "Read into NULL");
    ps_write8(b, kIn + 0x200, 13);
    h.r("cellRudpRead", {static_cast<u64>(c2), kIn, 16, 0, kIn + 0x200}, "Read with 13 bytes of information");
    h.r("cellRudpWrite", {static_cast<u64>(c2), kOut, 16, 0}, "Write before a connection");
    h.r("cellRudpWrite", {static_cast<u64>(c2), kOut, 0, 0}, "Write 0 bytes");
    h.r("cellRudpWrite", {static_cast<u64>(c2), 0, 16, 0}, "Write from NULL");
    h.r("cellRudpFlush", {static_cast<u64>(c2)}, "Flush before a connection");
    h.sizes(c2, "before a connection");
    h.remote(c2, "before a connection");
    h.ctx_status(c2, "bound, idle");
    h.events("the checks");

    // Polls.
    h.r("cellRudpPollCreate", {0}, "PollCreate(0)");
    const u64 p = h.r("cellRudpPollCreate", {1}, "PollCreate(1)");
    const u64 p2 = h.r("cellRudpPollCreate", {4}, "PollCreate(4)");
    h.r("cellRudpPollControl", {p, 1, static_cast<u64>(c2), 0xF}, "PollControl ADD");
    h.r("cellRudpPollControl", {p, 1, static_cast<u64>(c2), 0xF}, "PollControl ADD again");
    h.r("cellRudpPollControl", {p, 1, static_cast<u64>(c3), 0xF}, "PollControl ADD past its size");
    h.r("cellRudpPollControl", {p2, 1, static_cast<u64>(c2), 0xF}, "PollControl ADD to another set");
    h.r("cellRudpPollControl", {p, 1, static_cast<u64>(c + 1000), 0xF}, "PollControl ADD no context");
    h.r("cellRudpPollControl", {p + 1000, 1, static_cast<u64>(c2), 0xF}, "PollControl of no set");
    h.r("cellRudpPollControl", {p, 4, static_cast<u64>(c2), 0xF}, "PollControl op 4");
    h.r("cellRudpPollControl", {p, 0, static_cast<u64>(c2), 0xF}, "PollControl op 0");
    std::memset(b + kEvents, 0xEE, 64);
    h.r("cellRudpPollWait", {p, kEvents, 4, 0}, "PollWait(0)");
    h.o.line("PollWait(0): the events", hex(b + kEvents, 40));
    h.r("cellRudpPollWait", {p, kEvents, 4, 1000}, "PollWait(1 ms)");
    h.r("cellRudpPollWait", {p, 0, 4, 0}, "PollWait into NULL");
    h.r("cellRudpPollWait", {p, kEvents, 0, 0}, "PollWait for 0 events");
    h.r("cellRudpPollControl", {p, 2, static_cast<u64>(c2), 0x3}, "PollControl MODIFY");
    h.r("cellRudpPollControl", {p, 2, static_cast<u64>(c3), 0x3}, "PollControl MODIFY one not in it");
    std::memset(b + kEvents, 0xEE, 64);
    h.r("cellRudpPollWait", {p, kEvents, 4, 0}, "PollWait after MODIFY");
    h.o.line("PollWait after MODIFY: the events", hex(b + kEvents, 40));
    h.r("cellRudpPollControl", {p, 3, static_cast<u64>(c2), 0}, "PollControl REMOVE");
    h.r("cellRudpPollControl", {p, 3, static_cast<u64>(c2), 0}, "PollControl REMOVE again");
    std::memset(b + kEvents, 0xEE, 64);
    h.r("cellRudpPollWait", {p, kEvents, 4, 0}, "PollWait after REMOVE");
    h.o.line("PollWait after REMOVE: the events", hex(b + kEvents, 40));
    h.r("cellRudpPollCancel", {p}, "PollCancel");
    h.r("cellRudpPollWait", {p, kEvents, 4, 1000}, "PollWait after PollCancel");
    h.r("cellRudpPollWait", {p, kEvents, 4, 0}, "PollWait after that");
    h.r("cellRudpPollCancel", {p + 1000}, "PollCancel of no set");
    h.r("cellRudpPollDestroy", {p}, "PollDestroy");
    h.r("cellRudpPollDestroy", {p}, "PollDestroy again");
    h.r("cellRudpPollWait", {p, kEvents, 4, 0}, "PollWait on a destroyed set");
    h.status("with polls");
    h.r("cellRudpPollDestroy", {p2}, "PollDestroy the other");

    h.r("cellRudpNetReceived", {static_cast<u64>(-1), kIn, 32, kPeer, 16}, "NetReceived on socket -1");
    h.r("cellRudpNetReceived", {static_cast<u64>(udp), 0, 32, kPeer, 16}, "NetReceived from NULL");
    h.r("cellRudpNetReceived", {static_cast<u64>(udp), kIn, 0, kPeer, 16}, "NetReceived of 0 bytes");
    h.r("cellRudpNetReceived", {static_cast<u64>(udp), kIn, 32, 0, 16}, "NetReceived from no address");
    h.r("cellRudpNetReceived", {static_cast<u64>(udp), kIn, 32, kPeer, 0x23}, "NetReceived with 35 bytes of address");
    h.r("cellRudpNetReceived", {static_cast<u64>(udp + 100), kIn, 32, kPeer, 16}, "NetReceived on a socket not bound");
    h.r("cellRudpSetMaxSegmentSize", {1000}, "SetMaxSegmentSize with contexts");
    h.r("cellRudpEnableInternalIOThread", {0x4000, 1000}, "EnableInternalIOThread with contexts");
    h.r("cellRudpEnableInternalIOThread", {0x4000, 0xC00}, "EnableInternalIOThread at priority 0xc00");

    for (const s32 x : {c, c2, c3}) h.r("cellRudpTerminate", {static_cast<u64>(x)}, std::format("Terminate {}", x));
    h.r("cellRudpTerminate", {static_cast<u64>(c)}, "Terminate again");
    h.r("cellRudpTerminate", {static_cast<u64>(-1)}, "Terminate -1");
    h.pump("after the checks", 100ms);
    h.events("after the checks");
    h.settle("after the checks, lingering");
    h.events("after the checks, lingering");
    h.status("after the checks");
    h.r("cellRudpEnd", {}, "End");
    h.r("cellRudpEnd", {}, "End again");
    h.close(udp);
    h.close(udp2);
}

// A listener (with its cookies, or without) and a client in mux mode 2, driven by ProcessEvents: the handshake, data
// both ways, read information, flushing, polling, the client's reset, the end.
void stream_connection(Run& h, const std::string& w, u16 port, bool cookies, bool stream, u32 aggregation)
{
    u8* b = h.b;
    const s32 sa = h.socket(port), sb = h.socket(static_cast<u16>(port + 1));
    const s32 server = h.create(w + "the listener"), client = h.create(w + "the client");
    if (!cookies) h.set(server, 13, 0, w + "no cookies");
    if (stream)
        for (const s32 x : {server, client}) h.set(x, 8, 1, w + "STREAM");
    if (aggregation != 30)
        for (const s32 x : {server, client}) h.set(x, 11, aggregation, w + "AGGREGATION_TIMEOUT");
    h.r("cellRudpBind", {static_cast<u64>(server), static_cast<u64>(sa), 10, 2}, w + "Bind the listener");
    h.r("cellRudpBind", {static_cast<u64>(client), static_cast<u64>(sb), 20, 2}, w + "Bind the client");
    h.r("cellRudpListen", {static_cast<u64>(server), 4}, w + "Listen");
    sockaddr(b, kPeer, port);
    h.r("cellRudpInitiate", {static_cast<u64>(client), kPeer, 16, 10}, w + "Initiate");
    h.r("cellRudpInitiate", {static_cast<u64>(client), kPeer, 16, 10}, w + "Initiate again");
    h.ctx_status(client, w + "the client after Initiate");
    h.r("cellRudpWrite", {static_cast<u64>(client), kOut, 16, 0}, w + "Write while connecting");
    h.sizes(client, w + "connecting");
    h.pump(w + "the handshake", 400ms);
    h.events(w + "the handshake");
    h.r("cellRudpInitiate", {static_cast<u64>(client), kPeer, 16, 10}, w + "Initiate when connected");
    const s32 child = h.accept(server, w + "Accept");
    h.accept(server, w + "Accept again");
    h.ctx_status(client, w + "the client");
    h.ctx_status(server, w + "the listener");
    h.ctx_status(child, w + "the accepted context");
    h.remote(client, w + "the client");
    h.remote(child, w + "the accepted context");
    h.remote(child, w + "the accepted context, 8 bytes", 8);
    h.local(child, w + "the accepted context");
    h.sizes(client, w + "the client");
    h.sizes(child, w + "the accepted context");
    h.status(w + "connected");

    // Data both ways.
    h.write(client, 100, 1, w + "Write 100 bytes");
    h.write(client, 1, 2, w + "Write 1 byte");
    h.write(child, 700, 3, w + "Write 700 bytes back");
    h.sizes(client, w + "the client, written");
    h.pump(w + "the first data", 400ms);
    h.events(w + "the first data");
    h.status(w + "the first data");
    h.sizes(child, w + "the accepted context, data there");
    h.read(child, 50, w + "Read into 50 bytes");
    h.read(child, 2048, w + "Read the first message");
    h.read(child, 2048, w + "Read the second message");
    h.read(child, 2048, w + "Read with nothing there");
    h.read(client, 2048, w + "Read the answer", 0, 3);
    h.pump(w + "after reading", 300ms);
    h.events(w + "after reading");

    // Writes with flags; reads of their information.
    h.write(client, 64, 4, w + "Write latency critical", 0x08);
    h.write(client, 64, 5, w + "Write with a time stamp", 0x40);
    h.write(client, 40, 6, w + "Write aligned to 32", 0x10);
    h.write(client, 40, 7, w + "Write aligned to 64", 0x20);
    h.write(client, 0x542, 8, w + "Write the most there is");
    h.write(client, 0x543, 9, w + "Write a byte more");
    h.write(client, 5000, 10, w + "Write 5000 bytes");
    h.write(client, 20, 11, w + "Write with DONTWAIT", 0x01);
    h.write(client, 20, 12, w + "Write with flag 0x80", 0x80);
    h.sizes(client, w + "after the writes");
    h.pump(w + "the flagged data", 400ms);
    h.events(w + "the flagged data");
    h.status(w + "the flagged data");
    h.sizes(child, w + "the flagged data");
    for (u32 i = 0; i < 9; i++) {
        if (h.read(child, 0x2000, w + std::format("Read {}", i)) < 0) break;
    }
    h.status(w + "the flagged data read");
    h.ctx_status(client, w + "the client after the data");
    h.ctx_status(child, w + "the accepted context after the data");

    // Flushing, polling.
    h.write(client, 300, 13, w + "Write before Flush");
    h.r("cellRudpFlush", {static_cast<u64>(client)}, w + "Flush");
    h.r("cellRudpFlush", {static_cast<u64>(client)}, w + "Flush again");
    const u64 p = h.r("cellRudpPollCreate", {2}, w + "PollCreate");
    h.r("cellRudpPollControl", {p, 1, static_cast<u64>(child), 0xF}, w + "PollControl ADD the accepted context");
    h.r("cellRudpPollControl", {p, 1, static_cast<u64>(client), 0x7}, w + "PollControl ADD the client");
    h.r("cellRudpPollControl", {p, 1, static_cast<u64>(server), 0x1}, w + "PollControl ADD the listener");
    h.pump(w + "the flush", 400ms);
    h.events(w + "the flush");
    h.status(w + "the flush");
    std::memset(b + kEvents, 0xEE, 64);
    h.r("cellRudpPollWait", {p, kEvents, 4, 0}, w + "PollWait");
    h.o.line(w + "PollWait: the events", hex(b + kEvents, 40));
    h.read(child, 0x2000, w + "Read the flushed data");
    std::memset(b + kEvents, 0xEE, 64);
    h.r("cellRudpPollWait", {p, kEvents, 4, 0}, w + "PollWait after reading");
    h.o.line(w + "PollWait after reading: the events", hex(b + kEvents, 40));
    std::memset(b + kEvents, 0xEE, 64);
    h.r("cellRudpPollWait", {p, kEvents, 1, 0}, w + "PollWait for 1 event");
    h.o.line(w + "PollWait for 1 event: the events", hex(b + kEvents, 16));
    h.r("cellRudpPollControl", {p, 2, static_cast<u64>(child), 0x2}, w + "PollControl MODIFY");
    std::memset(b + kEvents, 0xEE, 64);
    h.r("cellRudpPollWait", {p, kEvents, 4, 0}, w + "PollWait after MODIFY");
    h.o.line(w + "PollWait after MODIFY: the events", hex(b + kEvents, 40));

    // The client's reset; the accepted context reads the end.
    h.write(child, 30, 14, w + "Write to the client before its end");
    h.pump(w + "data to the client", 300ms);
    h.r("cellRudpTerminate", {static_cast<u64>(client)}, w + "Terminate the client");
    h.ctx_status(client, w + "the client, terminated");
    h.r("cellRudpRead", {static_cast<u64>(client), kIn, 16, 0, 0}, w + "Read on the terminated client");
    h.pump(w + "the reset", 300ms);
    h.events(w + "the reset");
    std::memset(b + kEvents, 0xEE, 64);
    h.r("cellRudpPollWait", {p, kEvents, 4, 0}, w + "PollWait after the reset");
    h.o.line(w + "PollWait after the reset: the events", hex(b + kEvents, 40));
    h.ctx_status(child, w + "the accepted context after the reset");
    h.option(child, 14, 4, w + "LAST_ERROR after the reset");
    h.option(child, 14, 4, w + "LAST_ERROR again");
    h.read(child, 2048, w + "Read after the reset");
    h.r("cellRudpWrite", {static_cast<u64>(child), kOut, 16, 0}, w + "Write after the reset");
    h.sizes(child, w + "after the reset");
    h.remote(child, w + "after the reset");
    h.r("cellRudpPollDestroy", {p}, w + "PollDestroy");
    h.status(w + "after the reset");
    h.r("cellRudpTerminate", {static_cast<u64>(child)}, w + "Terminate the accepted context");
    h.r("cellRudpTerminate", {static_cast<u64>(server)}, w + "Terminate the listener");
    h.pump(w + "the end", 300ms);
    h.events(w + "the end");
    h.ctx_status(server, w + "the listener after Terminate");
    h.ctx_status(client, w + "the client after the end");
    h.settle(w + "lingering");
    h.events(w + "lingering");
    h.status(w + "the end");
    h.close(sa);
    h.close(sb);
}

// Two P2P contexts (mux mode 1, one vport) opening at once with Activate.
void p2p_connection(Run& h, const std::string& w, u16 port)
{
    u8* b = h.b;
    const s32 sa = h.socket(port), sb = h.socket(static_cast<u16>(port + 1));
    const s32 x = h.create(w + "the first"), y = h.create(w + "the second");
    for (const s32 c : {x, y}) h.set(c, 4, 1, w + "NODELAY");
    h.r("cellRudpBind", {static_cast<u64>(x), static_cast<u64>(sa), 7, 1}, w + "Bind the first");
    h.r("cellRudpBind", {static_cast<u64>(y), static_cast<u64>(sb), 7, 1}, w + "Bind the second");
    sockaddr(b, kPeer, static_cast<u16>(port + 1));
    h.r("cellRudpActivate", {static_cast<u64>(x), kPeer, 16}, w + "Activate the first");
    h.ctx_status(x, w + "the first after Activate");
    h.pump(w + "one side", 200ms);
    h.events(w + "one side");
    sockaddr(b, kPeer, port);
    h.r("cellRudpActivate", {static_cast<u64>(y), kPeer, 16}, w + "Activate the second");
    h.r("cellRudpActivate", {static_cast<u64>(y), kPeer, 16}, w + "Activate the second again");
    h.pump(w + "the handshake", 400ms);
    h.events(w + "the handshake");
    h.ctx_status(x, w + "the first");
    h.ctx_status(y, w + "the second");
    h.remote(x, w + "the first");
    h.local(y, w + "the second");
    h.write(x, 200, 20, w + "Write 200 bytes");
    h.write(y, 10, 21, w + "Write 10 bytes back");
    h.write(y, 11, 22, w + "Write 11 more");
    h.pump(w + "data", 400ms);
    h.events(w + "data");
    h.read(y, 0x1000, w + "Read on the second");
    h.read(x, 0x1000, w + "Read on the first");
    h.read(x, 0x1000, w + "Read on the first again");
    // A datagram lost: its retransmission.
    h.write(x, 50, 23, w + "Write what is lost");
    h.drop = {0};
    h.pump(w + "a loss", 1500ms);
    h.events(w + "a loss");
    h.read(y, 0x1000, w + "Read what was lost");
    h.ctx_status(x, w + "the first after the loss");
    h.ctx_status(y, w + "the second after the loss");
    h.r("cellRudpTerminate", {static_cast<u64>(x)}, w + "Terminate the first");
    h.pump(w + "the end", 300ms);
    h.events(w + "the end");
    h.ctx_status(y, w + "the second after the end");
    h.r("cellRudpTerminate", {static_cast<u64>(y)}, w + "Terminate the second");
    h.pump(w + "after the end", 100ms);
    h.events(w + "after the end");
    h.settle(w + "lingering");
    h.events(w + "lingering");
    h.status(w + "the end");
    h.close(sa);
    h.close(sb);
}

// Datagrams that are not right, and right ones to nobody.
void malformed(Run& h, u16 port)
{
    u8* b = h.b;
    const std::string w = "malformed: ";
    const s32 sa = h.socket(port);
    const s32 x = h.create(w + "a listener");
    h.set(x, 13, 0, w + "no cookies");
    h.r("cellRudpBind", {static_cast<u64>(x), static_cast<u64>(sa), 10, 2}, w + "Bind");
    h.r("cellRudpListen", {static_cast<u64>(x), 2}, w + "Listen");
    sockaddr(b, kFrom, static_cast<u16>(port + 1));
    const std::vector<std::pair<std::string, std::vector<u8>>> inputs = {
        {"one byte", {0xC0}},
        {"code 0", {0x00, 0x06, 0, 0, 0, 0}},
        {"length past the end", {0xC0, 0x20, 0, 20, 0, 10, 0x80, 0x10, 0, 0}},
        {"length 2", {0xC0, 0x02}},
        {"no header", {0xC0, 0x06, 0, 20, 0, 10}},
        {"a USR to no connection", {0xC0, 0x0A, 0, 20, 0, 10, 0x20, 0x10, 0x12, 0x34}},
        {"a USR with data to no connection", {0xC0, 0x0E, 0, 20, 0, 10, 0x00, 0x10, 0x12, 0x34, 1, 2, 3, 4}},
        {"a PRB to no connection", {0xC0, 0x0A, 0, 20, 0, 10, 0x40, 0x10, 0x12, 0x34}},
        {"a RST to no connection", {0xC0, 0x10, 0, 20, 0, 10, 0xC0, 0x10, 0x12, 0x34, 0, 0, 1, 2, 3, 4}},
        {"a SYN to another vport", {0xC0, 0x16, 0, 20, 0, 11, 0x80, 0x10, 0x12, 0x34, 0x03, 0x00, 0x01, 0x00, 0x01,
                                       0x00, 0xA1, 0xB2, 0xC3, 0xD4, 0x05, 0x82}},
        {"a SYN of version 0x200", {0xC0, 0x16, 0, 20, 0, 10, 0x80, 0x10, 0x12, 0x34, 0x03, 0x00, 0x02, 0x00, 0x02,
                                       0x00, 0xA1, 0xB2, 0xC3, 0xD4, 0x05, 0x82}},
        {"a SYN cut short", {0xC0, 0x10, 0, 20, 0, 10, 0x80, 0x10, 0x12, 0x34, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00}},
        {"a SYN in mux mode 1", {0x80, 0x14, 0, 10, 0x80, 0x10, 0x12, 0x34, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0xA1,
                                    0xB2, 0xC3, 0xD4, 0x05, 0x82}},
        {"a SYN without vports", {0x40, 0x12, 0x80, 0x10, 0x12, 0x34, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0xA1, 0xB2,
                                     0xC3, 0xD4, 0x05, 0x82}},
        {"two packets, the second malformed", {0xC0, 0x0A, 0, 20, 0, 10, 0x20, 0x10, 0x12, 0x34, 0x00, 0x08}},
        {"a bad option", {0xC0, 0x0E, 0, 20, 0, 10, 0x10, 0x10, 0x12, 0x34, 0x85, 0x08, 0, 0}},
    };
    for (const auto& [name, bytes] : inputs) {
        std::memcpy(b + kStage, bytes.data(), bytes.size());
        h.r("cellRudpNetReceived", {static_cast<u64>(sa), kStage, bytes.size(), kFrom, 16}, w + name);
        h.pump(w + name, 60ms);
    }
    h.events(w + "the end");
    h.ctx_status(x, w + "the listener");
    h.accept(x, w + "Accept");
    h.status(w + "the end");
    h.r("cellRudpTerminate", {static_cast<u64>(x)}, w + "Terminate");
    h.pump(w + "after Terminate", 100ms);
    h.settle(w + "lingering");
    h.events(w + "lingering");
    h.status(w + "after lingering");
    h.close(sa);
}

// Over the internal I/O thread, lv2's loopback sockets between the contexts: blocking contexts, timeouts, the hooks.
void io_thread(Run& h, u16 port)
{
    u8* b = h.b;
    const std::string w = "the I/O thread: ";
    h.pump_events = false;
    h.r("cellRudpInit", {0}, w + "Init");
    h.r("cellRudpSetEventHandler", {kNetOpd, kNetRing}, w + "SetEventHandler");
    h.r("cellRudpEnableInternalIOThread", {0x4000, 0xC00}, w + "EnableInternalIOThread at priority 0xc00");
    h.r("cellRudpEnableInternalIOThread", {0x4000, 1000}, w + "EnableInternalIOThread");
    h.r("cellRudpEnableInternalIOThread", {0x4000, 1000}, w + "EnableInternalIOThread again");
    h.r("cellRudpProcessEvents", {0}, w + "ProcessEvents");
    h.status(w + "started");
    const s32 sa = h.socket(port), sb = h.socket(static_cast<u16>(port + 1));
    h.r("cellRudpNetReceived", {static_cast<u64>(sa), kIn, 32, kPeer, 16}, w + "NetReceived");
    const s32 server = h.create(w + "the listener"), client = h.create(w + "the client");
    h.r("cellRudpSetMaxSegmentSize", {1000}, w + "SetMaxSegmentSize with contexts");
    h.set(client, 7, 0, w + "the client blocks");
    h.set(client, 15, 150, w + "READ_TIMEOUT");
    h.set(client, 17, 2000, w + "FLUSH_TIMEOUT");
    h.r("cellRudpBind", {static_cast<u64>(server), static_cast<u64>(sa), 10, 2}, w + "Bind the listener");
    h.r("cellRudpBind", {static_cast<u64>(client), static_cast<u64>(sb), 20, 2}, w + "Bind the client");
    h.r("cellRudpListen", {static_cast<u64>(server), 4}, w + "Listen");
    sockaddr(b, kPeer, port);
    h.datagram = 0;
    h.per_direction.clear();
    h.r("cellRudpInitiate", {static_cast<u64>(client), kPeer, 16, 10}, w + "Initiate, blocking");
    h.pump(w + "the handshake", 300ms);
    h.events(w + "the handshake");
    h.ctx_status(client, w + "the client");
    const s32 child = h.accept(server, w + "Accept");
    h.ctx_status(child, w + "the accepted context");
    const auto t0 = std::chrono::steady_clock::now();
    h.r("cellRudpRead", {static_cast<u64>(client), kIn, 64, 0, 0}, w + "Read with nothing there, blocking");
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    h.o.line(w + "Read with nothing there: waited 100 ms or more", waited >= 100ms ? "yes" : "no");
    h.o.note(w + std::format("Read with nothing there waited {} ms", waited.count()));
    h.r("cellRudpRead", {static_cast<u64>(client), kIn, 64, 1, 0}, w + "Read with DONTWAIT");
    h.write(child, 500, 30, w + "Write to the client");
    h.datagram = 0;
    h.per_direction.clear();
    h.read(client, 0x1000, w + "Read, blocking");
    h.write(client, 900, 31, w + "Write to the accepted context");
    h.datagram = 0;
    h.per_direction.clear();
    h.r("cellRudpFlush", {static_cast<u64>(client)}, w + "Flush, blocking");
    h.pump(w + "the data", 300ms);
    h.events(w + "the data");
    h.read(child, 0x1000, w + "Read on the accepted context");
    const u64 p = h.r("cellRudpPollCreate", {2}, w + "PollCreate");
    h.r("cellRudpPollControl", {p, 1, static_cast<u64>(child), 0x1}, w + "PollControl ADD");
    std::memset(b + kEvents, 0xEE, 64);
    h.r("cellRudpPollWait", {p, kEvents, 2, 20000}, w + "PollWait(20 ms) with nothing there");
    h.write(client, 10, 32, w + "Write 10 bytes");
    std::memset(b + kEvents, 0xEE, 64);
    h.r("cellRudpPollWait", {p, kEvents, 2, 2000000}, w + "PollWait(2 s)");
    h.o.line(w + "PollWait(2 s): the events", hex(b + kEvents, 24));
    h.read(child, 0x1000, w + "Read the 10 bytes");
    h.r("cellRudpPollDestroy", {p}, w + "PollDestroy");
    h.pump(w + "polling", 300ms);
    h.r("cellRudpTerminate", {static_cast<u64>(child)}, w + "Terminate the accepted context");
    h.pump(w + "the reset", 300ms);
    h.events(w + "the reset");
    h.read(client, 0x1000, w + "Read after the reset");
    h.ctx_status(client, w + "the client after the reset");
    h.status(w + "the end");
    h.r("cellRudpEnd", {}, w + "End with contexts");
    h.pump(w + "after End", 100ms);
    h.events(w + "after End");
    h.close(sa);
    h.close(sb);
}

Observed half(const Setup& s, bool sony_half)
{
    Run h(s, sony_half);
    write_handlers(h.b);
    sockaddr(h.b, kPeer, 7001);
    // PSPOCKET_COMPARE_ONLY=<part> (before, basics, cookies, stream, p2p, malformed, io): that part alone.
    if (selected("before")) before_init(h);
    if (selected("basics")) basics(h);
    // The connections' sockets go through libnet in Sony's librudp: the network comes up first, as a game's does.
    ps_write32(h.b, kNetParam, kNetPool);
    ps_write32(h.b, kNetParam + 4, 0x20000);
    ps_write32(h.b, kNetParam + 8, 0);
    h.o.line("sys_net_initialize_network_ex", answer(h.t.call("sys_net_initialize_network_ex", kNetParam)));
    h.r("cellRudpInit", {0}, "Init for the connections");
    h.r("cellRudpSetEventHandler", {kNetOpd, kNetRing}, "SetEventHandler for the connections");
    if (selected("cookies")) stream_connection(h, "a listener with cookies: ", 7100, true, false, 30);
    if (selected("stream")) stream_connection(h, "a listener without cookies, stream, no aggregation: ", 7200, false, true, 0);
    if (selected("p2p")) p2p_connection(h, "P2P: ", 7300);
    if (selected("malformed")) malformed(h, 7400);
    h.status("before End");
    h.r("cellRudpEnd", {}, "End after the connections");
    if (selected("io")) io_thread(h, 7500);
    h.o.line("sys_net_finalize_network", answer(h.t.call("sys_net_finalize_network")));
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
        // pspocket's own half: Sony's answers as the comparison found them.
        CHECK(o.value("cellRudpCreateContext before Init") == "0x80770001");
        CHECK(o.value("Init again") == "0x80770002");
        CHECK(o.value("GetMaxSegmentSize: the size") == "hex:0582eeee");
        CHECK(o.value("CreateContext: the ID") == "0");
        CHECK(o.value("CreateContext again: the ID") == "1");
        CHECK(o.value("GetOption(1) in 4 bytes: the value") == "hex:00000542eeeeeeee");
        CHECK(o.value("GetOption(12) in 4 bytes: the value") == "hex:00000001eeeeeeee");
        CHECK(o.value("Listen without an event handler") == "0x80770022");
        CHECK(o.value("a listener with cookies: Read the first message") == "0x64");
        CHECK(o.value("a listener with cookies: Read into 50 bytes") == "0x8077001a");
        CHECK(o.value("a listener with cookies: Read with nothing there") == "0x80770016");
        CHECK(o.value("a listener with cookies: Read the answer") == "0x2bc");
        CHECK(o.value("the I/O thread: EnableInternalIOThread again") == "0x80770010");
        CHECK(o.lines.size() > 1500);
    });
}
