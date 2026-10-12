#pragma once

// cellRudp's wire format (D-916, read from Sony's librudp), kept apart so the tests can make and read packets; the
// library is runtime/hle/src/cellRudp.cpp. A peer may be a real PS3 running Sony's librudp: every byte below is as
// librudp writes and reads it.
//
// A UDP (or UDP P2P) datagram holds one or more packets, each:
//   u16 prefix: bits 15-14 the vport code (1: no vport, 2: one, 3: two), bits 10-0 the packet's length in bytes (the
//       prefix, the vports, the header and the payload); bits 13-11 are 0
//   u16 vport (code 2: the vport both sides share, a P2P context's) or u16 source vport, u16 destination vport (code 3)
//   the header, then the payload (the rest of the packet).
// A context bound in mux mode 1 (P2P) sends code 2, one in mux mode 2 code 3.
//
// The header (big-endian):
//   u8  type << 6 | flags      type: 0 USR (data, or an acknowledgement alone), 1 PRB (probe, keep-alive), 2 SYN,
//                              3 RST; flags: 0x20 an acknowledgement follows, 0x10 options follow the header,
//                              0x08 latency critical, 0x04 an acknowledgement delay follows, 0x02 a stream segment that
//                              is not the last of what was written
//   u8  window                 the receive slots (segments) the sender has free, in a byte (encode_window)
//   u16 sequence number
//   [u16 acknowledgement: the next sequence number the sender expects]          flags & 0x20
//   [u16 acknowledgement delay in milliseconds]                                  flags & 0x04 (needs 0x20)
//   SYN: u16 parameters (0x800 stream, 0x200 delivery critical, 0x100 order critical, low 4 bits a level),
//        u16 version (0x100), u16 lowest version (0x100), u32 the connection's token, u16 the segment size
//   RST: u16 the reason (its low byte), u32 the token of the connection it ends
//   options (flags & 0x10), each u8 kind (bit 7: the last one) and u8 length, then its bytes; kind 0 is a single
//   padding byte without a length:
//        1 a listener's cookie (1 to 32 bytes)        2 u16 segment size (not on a SYN)
//        5 selective acknowledgements: u16 first, u16 past the last, per block (up to 8)
//        10 u32 retransmission: count (bits 31-24, 255 at most) and delay since the first sending in ms (23-0)
//        11 u32 the sender's millisecond clock when the segment went (a write with the time stamp flag)
//   padding (kind 0) so that the payload starts on a 4, 8 or 16-byte boundary (a write's ALIGN flags).

#include <array>
#include <functional>
#include <optional>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::rudp {

enum : u8 { kTypeUsr = 0, kTypePrb = 1, kTypeSyn = 2, kTypeRst = 3 };
enum : u8 {
    kFlagAck = 0x20,
    kFlagOptions = 0x10,
    kFlagLatency = 0x08,
    kFlagDelay = 0x04,
    kFlagMore = 0x02,
};
enum : u8 { kSynStream = 1, kSynDelivery = 2, kSynOrder = 4 }; // Header::syn
enum : u8 { kOptCookie = 1, kOptMss = 2 };                     // Header::opts
enum : u8 { kExtRetransmission = 1, kExtTimestamp = 2 };       // Header::ext

constexpr u16 kVersion = 0x100;

struct Header {
    u8 type = kTypeUsr;
    u8 flags = 0;
    u8 window = 0; // encoded
    u16 seq = 0, ack = 0, delay = 0;
    // SYN
    u8 syn = 0, level = 0;
    u16 version = kVersion, lowest = kVersion;
    u32 token = 0; // SYN and RST
    u16 mss = 0;   // SYN, and the option
    u8 code = 0;   // RST
    // Options.
    u8 opts = 0, ext = 0;
    std::array<u8, 32> cookie{};
    u32 cookie_len = 0;
    std::array<std::array<u16, 2>, 8> sack{};
    u32 nsack = 0;
    u32 retransmission = 0, stamp = 0;
    u8 align = 0; // 4, 8 or 16: the payload's boundary (0: none)
};

// The window's byte: up to 0x3f slots as they are, more with fewer bits of precision (librudp's encoding).
inline u8 encode_window(u16 v)
{
    if (v & 0xF000) return 0xFF;
    if (v & 0xFC00) return static_cast<u8>((v >> 6) | 0xC0);
    if (v & 0xFF00) return static_cast<u8>((v >> 4) | 0x80);
    if (!(v & 0xFFC0)) return static_cast<u8>(v);
    return static_cast<u8>((v >> 2) | 0x40);
}
inline u16 decode_window(u8 b) { return static_cast<u16>((b & 0x3F) << ((b >> 5) & 6)); }

// Appends the header to `out`; `base_align` is the offset of out's first byte from a 16-byte boundary in the buffer
// the packet is built in (the padding aligns what follows in that buffer).
void encode(const Header& h, std::vector<u8>& out, u32 base_align = 0);
// Reads a header from p[0..n): its length (the payload follows), or nothing when it is malformed.
std::optional<u32> decode(const u8* p, u32 n, Header& h);

// One datagram the library sends: on socket `soc` to the sys_net_sockaddr `to`.
struct Packet {
    s32 soc = 0;
    std::array<u8, 16> to{};
    std::vector<u8> bytes;
};

// The test hook on the socket path: called for every datagram the library hands to lv2 or to the event handler, on the
// sending thread. It appends to `send` what goes out now: nothing (lost), the datagram (as it is), or datagrams it held
// back before (reordered). Each process's library has its own; empty: none.
using SendFilter = std::function<void(Packet packet, std::vector<Packet>& send)>;
void set_send_filter(SendFilter filter); // the current process's

} // namespace pspocket::hle::rudp
