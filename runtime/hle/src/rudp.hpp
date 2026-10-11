#pragma once

// cellRudp's wire format and its test hook (D-631). The format is pspocket's own: Sony's librudp speaks another one
// that is not known, so pspocket's RUDP talks only to pspocket's (a real PS3 needs Sony's format; PSN play is gone).
// It is here so the tests can read and make packets; the library is runtime/hle/src/cellRudp.cpp.
//
// Every packet is one UDP (or UDP P2P) datagram: a 32-byte header, big-endian, then the payload.
//   +0  u16 magic 0x5052 ("PR")      +2  u8 version (1)        +3  u8 type (Type below)
//   +4  u8  flags (kFlag*)           +5  u8 reason (RST: kRst*; else 0)
//   +6  u8  retries (DATA: times the segment was sent before this one)          +7  u8 0
//   +8  u16 source virtual port      +10 u16 destination virtual port
//   +12 u32 sequence number (SYN, SYN-ACK, DATA, FIN: the segment's; one sequence space, SYN and FIN take one each)
//   +16 u32 acknowledgement: the next sequence number the sender expects (the highest seen + 1 without delivery
//           guarantees)
//   +20 u32 selective acknowledgement: bit i, segment ack + 1 + i has arrived
//   +24 u32 window: the bytes the sender of this packet can still take for reading
//   +28 u32 time stamp: the sender's guest clock in microseconds (DATA: at the segment's first sending)
// SYN and SYN-ACK carry the connection's parameters (12 bytes): u8 stream, u8 delivery critical, u8 order critical,
// u8 0, u16 the sender's maximum segment size (header and payload), u16 0, u32 its receive buffer.

#include <array>
#include <functional>
#include <optional>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/types.hpp"

namespace pspocket::hle::rudp {

inline constexpr u16 kMagic = 0x5052;
inline constexpr u8 kVersion = 1;
inline constexpr u32 kHeaderSize = 32;
inline constexpr u32 kSynSize = 12;

enum class Type : u8 { Syn = 1, SynAck = 2, Ack = 3, Data = 4, Probe = 5, Fin = 6, Rst = 7 };

enum : u8 {
    kFlagSoft = 0x01,    // a SYN from cellRudpActivate: dropped, not refused, where nothing waits for it
    kFlagLatency = 0x02, // DATA written with CELL_RUDP_MSG_LATENCY_CRITICAL
};

// Why a RST was sent.
enum : u8 { kRstRefused = 1, kRstVersion = 2, kRstTransport = 3, kRstQuality = 4, kRstReset = 5 };

struct Header {
    u8 version = kVersion;
    Type type = Type::Ack;
    u8 flags = 0, reason = 0, retries = 0;
    u16 src = 0, dst = 0;
    u32 seq = 0, ack = 0, sack = 0, window = 0, stamp = 0;
};

inline void encode(const Header& h, u8* p)
{
    ps_st16(p, kMagic);
    p[2] = h.version;
    p[3] = static_cast<u8>(h.type);
    p[4] = h.flags;
    p[5] = h.reason;
    p[6] = h.retries;
    p[7] = 0;
    ps_st16(p + 8, h.src);
    ps_st16(p + 10, h.dst);
    ps_st32(p + 12, h.seq);
    ps_st32(p + 16, h.ack);
    ps_st32(p + 20, h.sack);
    ps_st32(p + 24, h.window);
    ps_st32(p + 28, h.stamp);
}

// The header of a packet of `n` bytes: none when it is too short or lacks the magic (not an RUDP packet). The version
// and the type are as they came (the receiver checks them).
inline std::optional<Header> decode(const u8* p, usize n)
{
    if (n < kHeaderSize || ps_ld16(p) != kMagic) return std::nullopt;
    Header h;
    h.version = p[2];
    h.type = static_cast<Type>(p[3]);
    h.flags = p[4];
    h.reason = p[5];
    h.retries = p[6];
    h.src = ps_ld16(p + 8);
    h.dst = ps_ld16(p + 10);
    h.seq = ps_ld32(p + 12);
    h.ack = ps_ld32(p + 16);
    h.sack = ps_ld32(p + 20);
    h.window = ps_ld32(p + 24);
    h.stamp = ps_ld32(p + 28);
    return h;
}

// One packet the library sends: on socket `soc` to the sys_net_sockaddr `to`.
struct Packet {
    s32 soc = 0;
    std::array<u8, 16> to{};
    std::vector<u8> bytes;
};

// The test hook on the socket path (D-631): called for every packet the library sends, on the sending thread, with no
// lock of the library held. It appends to `send` what goes out now: nothing (the packet is lost), the packet (as it
// is), or packets it held back before as well (reordered). Each process's library has its own; empty: none.
using SendFilter = std::function<void(Packet packet, std::vector<Packet>& send)>;
void set_send_filter(SendFilter filter); // the current process's

} // namespace pspocket::hle::rudp
