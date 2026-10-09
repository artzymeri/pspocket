// lv2's network calls (sys_net_bnet_* and the rest, 700..726), for a console whose network cable is unplugged
// (D-523). Online play is a non-goal (D-024): libnet has to come up and behave as on a console without a link,
// consistently with cellNetCtl (state Disconnected, no IP address). References: RPCS3 rpcs3/Emu/Cell/lv2/sys_net.cpp
// and .h, lv2/sys_net/lv2_socket*.cpp and sys_net_helpers.cpp (argument checks, error codes, socket IDs, select, the
// socket information), BSD's socket layer for what a host would answer behind RPCS3. Each call answers a value or
// -SYS_NET_E* (libnet makes that SYS_NET_ERROR_*).
//
// The offline model, a deviation from RPCS3 (which gives the game host sockets):
// - No packet leaves the process. Sockets live in a table here, numbered as RPCS3's lv2_socket IDs: 24 to 1023, in
//   increasing order until the range is used up, then the lowest free one (IdManager allocate_id).
// - socket, bind, listen, setsockopt, getsockopt, getsockname, getpeername, shutdown, close, select, poll and the
//   socket information keep a console's socket state, with RPCS3's argument checks and error codes. With the cable
//   unplugged the only interface is the loopback one: bind takes INADDR_ANY, 127.0.0.1 and (datagram sockets)
//   multicast groups; any other address gives EADDRNOTAVAIL, as no interface has it.
// - connect, sendto and send to an address outside 127/8 fail with SYS_NET_EADDRNOTAVAIL: RPCS3's answer while its
//   network is disconnected (lv2_socket_native::connect/sendto, for public addresses; here nothing is reachable).
// - Loopback works in memory, between the game's own sockets. A UDP datagram (a P2P one by virtual port) reaches the
//   socket bound to its destination, within that socket's SO_RCVBUF (else it is dropped, as BSD drops it). A TCP
//   connect to 127/8 reaches a listening socket of the game (ECONNREFUSED when there is none or its backlog is full)
//   and completes at once; the connection carries bytes both ways, up to SO_RCVBUF + SO_SNDBUF in flight per
//   direction, with BSD's EOF and EPIPE (a close with unread data gives the peer EOF, not a reset). P2P stream
//   sockets and raw sockets have no loopback peer.
// - recvfrom, recvmsg and accept on a non-blocking socket (SO_NBIO, MSG_DONTWAIT) answer EWOULDBLOCK. On a blocking
//   socket they wait as a console's do: for loopback data, the socket's shutdown, a close or sys_net_abort (EINTR,
//   RPCS3's abort_socket), its SO_RCVTIMEO (EWOULDBLOCK, as RPCS3; accept honours it as well), or the process exit. A
//   console without a link never receives anything else, so a blocking call without a timeout on a socket the game
//   doesn't feed itself sleeps for good, as it does there. The wait shows in the thread report as a network wait, and
//   the first one on each socket is logged.
// - select and poll wait up to their timeout the same way; exceptfds is never set (as RPCS3), and select's count is of
//   sockets with an event, not of bits (as RPCS3).
// - infoctl 9 gives the resolver configuration (/etc/resolv.conf for libnet): empty, no name server without a link,
//   unless the system set one (infoctl 52). The dumps, sys_net_control, sysctl and eurus_post_command answer CELL_OK
//   with nothing, as RPCS3's stubs; ioctl keeps the test parameters libnet sets and reads back.
//
// Other deviations: pointers are checked before use (D-042): one that is not guest memory is logged and the call fails
// with SYS_NET_EFAULT, the BSD answer (RPCS3 faults). What RPCS3 asserts on or throws for (stream and datagram sockets
// of another family or protocol, unknown flags, connect, listen or accept on P2P datagram and raw sockets) is logged
// as an error and refused. The socket information reports the bytes queued for reading (RPCS3 0).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "internal.hpp"

namespace pspocket::lv2 {
namespace {

constexpr std::string_view kNetChan = "sys_net";
using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------------------------------------------- constants
// RPCS3 lv2/sys_net.h (its SYS_NET_ names; the numbers are BSD's).

enum : s32 {
    SYS_NET_ENOENT = 2,
    SYS_NET_EINTR = 4,
    SYS_NET_ENXIO = 6,
    SYS_NET_EBADF = 9,
    SYS_NET_ENOMEM = 12,
    SYS_NET_EACCES = 13,
    SYS_NET_EFAULT = 14,
    SYS_NET_EBUSY = 16,
    SYS_NET_EINVAL = 22,
    SYS_NET_EMFILE = 24,
    SYS_NET_ENOSPC = 28,
    SYS_NET_EPIPE = 32,
    SYS_NET_EDOM = 33, // not in RPCS3's list: BSD's answer to a bad timeval for SO_RCVTIMEO and SO_SNDTIMEO
    SYS_NET_EWOULDBLOCK = 35, // = SYS_NET_EAGAIN
    SYS_NET_EALREADY = 37,
    SYS_NET_EDESTADDRREQ = 39,
    SYS_NET_EMSGSIZE = 40,
    SYS_NET_EPROTONOSUPPORT = 43,
    SYS_NET_EOPNOTSUPP = 45,
    SYS_NET_EAFNOSUPPORT = 47,
    SYS_NET_EADDRINUSE = 48,
    SYS_NET_EADDRNOTAVAIL = 49,
    SYS_NET_ENOBUFS = 55,
    SYS_NET_EISCONN = 56,
    SYS_NET_ENOTCONN = 57,
    SYS_NET_ECONNREFUSED = 61,
};

enum : s32 {
    SYS_NET_SOCK_STREAM = 1,
    SYS_NET_SOCK_DGRAM = 2,
    SYS_NET_SOCK_RAW = 3,
    SYS_NET_SOCK_DGRAM_P2P = 6,
    SYS_NET_SOCK_STREAM_P2P = 10,
};

enum : s32 {
    SYS_NET_AF_UNSPEC = 0,
    SYS_NET_AF_INET = 2,
    SYS_NET_AF_INET6 = 24,
};

enum : s32 {
    SYS_NET_IPPROTO_IP = 0,
    SYS_NET_IPPROTO_TCP = 6,
    SYS_NET_IPPROTO_UDP = 17,
};

enum : s32 {
    SYS_NET_SOL_SOCKET = 0xffff,
    SYS_NET_SO_REUSEADDR = 0x0004,
    SYS_NET_SO_KEEPALIVE = 0x0008,
    SYS_NET_SO_BROADCAST = 0x0020,
    SYS_NET_SO_LINGER = 0x0080,
    SYS_NET_SO_REUSEPORT = 0x0200,
    SYS_NET_SO_USECRYPTO = 0x1000,
    SYS_NET_SO_USESIGNATURE = 0x2000,
    SYS_NET_SO_SNDBUF = 0x1001,
    SYS_NET_SO_RCVBUF = 0x1002,
    SYS_NET_SO_SNDLOWAT = 0x1003,
    SYS_NET_SO_RCVLOWAT = 0x1004,
    SYS_NET_SO_SNDTIMEO = 0x1005,
    SYS_NET_SO_RCVTIMEO = 0x1006,
    SYS_NET_SO_ERROR = 0x1007,
    SYS_NET_SO_TYPE = 0x1008,
    SYS_NET_SO_NBIO = 0x1100,
};

enum : s32 {
    SYS_NET_TCP_NODELAY = 1,
    SYS_NET_TCP_MAXSEG = 2,
};

enum : s32 {
    SYS_NET_IP_HDRINCL = 2,
    SYS_NET_IP_TOS = 3,
    SYS_NET_IP_TTL = 4,
    SYS_NET_IP_MULTICAST_IF = 9,
    SYS_NET_IP_MULTICAST_TTL = 10,
    SYS_NET_IP_MULTICAST_LOOP = 11,
    SYS_NET_IP_ADD_MEMBERSHIP = 12,
    SYS_NET_IP_DROP_MEMBERSHIP = 13,
    SYS_NET_IP_TTLCHK = 23,
    SYS_NET_IP_MAXTTL = 24,
    SYS_NET_IP_DONTFRAG = 26,
};

enum : s32 {
    SYS_NET_MSG_PEEK = 0x2,
    SYS_NET_MSG_TRUNC = 0x10,
    SYS_NET_MSG_WAITALL = 0x40,
    SYS_NET_MSG_DONTWAIT = 0x80,
    SYS_NET_MSG_USECRYPTO = 0x400,
    SYS_NET_MSG_USESIGNATURE = 0x800,
};
// The flags sys_net_bnet_recvfrom and sys_net_bnet_sendto accept (RPCS3 throws on any other).
constexpr s32 kRecvFlags =
    SYS_NET_MSG_PEEK | SYS_NET_MSG_DONTWAIT | SYS_NET_MSG_WAITALL | SYS_NET_MSG_USECRYPTO | SYS_NET_MSG_USESIGNATURE;
constexpr s32 kSendFlags = SYS_NET_MSG_DONTWAIT | SYS_NET_MSG_WAITALL | SYS_NET_MSG_USECRYPTO | SYS_NET_MSG_USESIGNATURE;

enum : s32 {
    SYS_NET_SHUT_RD = 0,
    SYS_NET_SHUT_WR = 1,
    SYS_NET_SHUT_RDWR = 2,
};

// sys_net_pollfd events (RPCS3 lv2/sys_net.h).
enum : u16 {
    SYS_NET_POLLIN = 0x0001,
    SYS_NET_POLLOUT = 0x0004,
    SYS_NET_POLLNVAL = 0x0020,
};

// sys_net_sockinfo_t::state.
enum : s32 {
    SYS_NET_STATE_UNKNOWN = 0,
    SYS_NET_STATE_CREATED = 2,
    SYS_NET_STATE_OPENED = 3,
    SYS_NET_STATE_LISTEN = 4,
    SYS_NET_STATE_ESTABLISHED = 7,
};

// The kernel's TCP states (BSD tcp_fsm.h), as lv2 reports them in a socket list; libnet maps them to the SYS_NET_STATE
// numbers (sys_net_get_sockinfo).
enum : u32 {
    TCPS_CLOSED = 0,
    TCPS_LISTEN = 1,
    TCPS_ESTABLISHED = 4,
    TCPS_NONE = 0xFFFFFFFF, // not a TCP socket: past libnet's table, so OPENED
};

// h_errno (BSD netdb.h; cellHttp's CELL_HTTP_ERROR_NET_RESOLVER_* carry the same numbers).
enum : s32 {
    SYS_NET_NETDB_INTERNAL = -1,
    SYS_NET_NETDB_SUCCESS = 0,
    SYS_NET_HOST_NOT_FOUND = 1,
    SYS_NET_TRY_AGAIN = 2,
};

// sys_net_abort's types (RPCS3 sys_net_abort) and flags.
enum : s32 {
    kAbortSocket = 0,
    kAbortResolver = 1,
    kAbortThread = 2,     // libnet: a thread's context, before a resolver call and after it
    kAbortThreadFree = 3, // libnet: a thread's context freed
    kAbortAll = 4,        // libnet: sys_net_finalize_network
    SYS_NET_ABORT_STRICT_CHECK = 1,
};

constexpr u32 kIdBase = 24, kIdCount = 1000; // RPCS3 lv2_socket id_base, id_count
constexpr u32 kLoopback = 0x7F000001;        // 127.0.0.1
constexpr u32 kInaddrBroadcast = 0xFFFFFFFF;
constexpr u16 kNpPort = 3658;                // SCE_NP_PORT: the UDP port of P2P sockets
constexpr u32 kSomaxconn = 128;              // BSD's cap on a listen backlog
constexpr u32 kUdpMaxPayload = 65507;        // 65535 - IP and UDP headers
constexpr u32 kP2pMaxPayload = 65529;        // RPCS3 lv2_socket_p2p::sendto: 65535 - VPORT_P2P_HEADER_SIZE
// RPCS3 lv2_socket_native::set_default_buffers, "the default PS3 values".
constexpr u32 kDefaultSndBuf = 131072, kDefaultRcvBufStream = 65535, kDefaultRcvBufDgram = 9216;
// Ports for an implicit or port-0 bind: FreeBSD's IPPORT_HIFIRSTAUTO..IPPORT_HILASTAUTO (the PS3's own range is not
// known).
constexpr u32 kEphemeralFirst = 49152, kEphemeralCount = 16384;
constexpr u16 kP2pFirstVport = 30000; // RPCS3: the first virtual port bind and connect hand out

std::string_view errno_name(s32 e)
{
    switch (e) {
    case SYS_NET_ENOENT: return "SYS_NET_ENOENT";
    case SYS_NET_EINTR: return "SYS_NET_EINTR";
    case SYS_NET_ENXIO: return "SYS_NET_ENXIO";
    case SYS_NET_EBADF: return "SYS_NET_EBADF";
    case SYS_NET_ENOMEM: return "SYS_NET_ENOMEM";
    case SYS_NET_EACCES: return "SYS_NET_EACCES";
    case SYS_NET_EFAULT: return "SYS_NET_EFAULT";
    case SYS_NET_EBUSY: return "SYS_NET_EBUSY";
    case SYS_NET_EINVAL: return "SYS_NET_EINVAL";
    case SYS_NET_EMFILE: return "SYS_NET_EMFILE";
    case SYS_NET_ENOSPC: return "SYS_NET_ENOSPC";
    case SYS_NET_EPIPE: return "SYS_NET_EPIPE";
    case SYS_NET_EDOM: return "SYS_NET_EDOM";
    case SYS_NET_EWOULDBLOCK: return "SYS_NET_EWOULDBLOCK";
    case SYS_NET_EALREADY: return "SYS_NET_EALREADY";
    case SYS_NET_EDESTADDRREQ: return "SYS_NET_EDESTADDRREQ";
    case SYS_NET_EMSGSIZE: return "SYS_NET_EMSGSIZE";
    case SYS_NET_EPROTONOSUPPORT: return "SYS_NET_EPROTONOSUPPORT";
    case SYS_NET_EOPNOTSUPP: return "SYS_NET_EOPNOTSUPP";
    case SYS_NET_EAFNOSUPPORT: return "SYS_NET_EAFNOSUPPORT";
    case SYS_NET_EADDRINUSE: return "SYS_NET_EADDRINUSE";
    case SYS_NET_EADDRNOTAVAIL: return "SYS_NET_EADDRNOTAVAIL";
    case SYS_NET_ENOBUFS: return "SYS_NET_ENOBUFS";
    case SYS_NET_EISCONN: return "SYS_NET_EISCONN";
    case SYS_NET_ENOTCONN: return "SYS_NET_ENOTCONN";
    case SYS_NET_ECONNREFUSED: return "SYS_NET_ECONNREFUSED";
    }
    return "?";
}

// What a kernel call returns, for logs: the value, or the errno.
std::string result_text(s32 r)
{
    if (r >= 0) return std::to_string(r);
    return std::format("errno {}", errno_name(-r));
}

std::string_view type_name(s32 type)
{
    switch (type) {
    case SYS_NET_SOCK_STREAM: return "STREAM";
    case SYS_NET_SOCK_DGRAM: return "DGRAM";
    case SYS_NET_SOCK_RAW: return "RAW";
    case SYS_NET_SOCK_DGRAM_P2P: return "DGRAM-P2P";
    case SYS_NET_SOCK_STREAM_P2P: return "STREAM-P2P";
    }
    return "?";
}

std::string ip_text(u32 a) { return std::format("{}.{}.{}.{}", a >> 24, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF); }

// Destinations the loopback interface takes: 127/8, and INADDR_ANY, which BSD's connect and sendto send to the primary
// local address (the loopback one here).
bool loopback(u32 addr) { return (addr >> 24) == 127 || addr == 0; }
u32 normalized(u32 addr) { return addr ? addr : kLoopback; }
bool multicast(u32 addr) { return (addr >> 28) == 0xE; }


// ------------------------------------------------------------------------------------------------- guest memory

// A pointer argument checked before use (D-042): one that is not guest memory is logged and the call fails with EFAULT.
bool usable(PpuThread& t, u32 addr, u64 size, std::string_view what)
{
    if (!size || t.kernel.impl().guest.valid(addr, size)) return true;
    PS_LOG_ERROR(kNetChan, "{} ({:#x}, {} bytes) is not guest memory (thread {:#x}, cia {:#x})", what, addr, size, t.id,
        t.ctx ? t.ctx->cia : 0);
    return false;
}

struct GuestText {
    std::string text;
    bool terminated = false; // a NUL within the bytes read
};

// The NUL-terminated string at `addr`, at most `max` characters (terminated false when longer); nullopt (logged) when
// it runs into memory that isn't guest memory first.
std::optional<GuestText> text_arg(PpuThread& t, u32 addr, u32 max, std::string_view what)
{
    const Guest& g = t.kernel.impl().guest;
    GuestText out;
    for (u32 i = 0;; i++) {
        const u64 a = u64{addr} + i;
        if (!g.valid(static_cast<u32>(a), 1) || a >= GuestMemory::kGuestSize) {
            PS_LOG_ERROR(kNetChan, "{} ({:#x}) is not a string in guest memory", what, addr);
            return std::nullopt;
        }
        const char c = static_cast<char>(g.r8(static_cast<u32>(a)));
        if (!c) {
            out.terminated = true;
            return out;
        }
        if (i == max) return out;
        out.text.push_back(c);
    }
}

// sys_net_sockaddr_in and sys_net_sockaddr_in_p2p (16 bytes): sin_len, sin_family, be16 port, be32 address, then a
// be16 virtual port (P2P) and zeros.
constexpr u32 kSockaddrSize = 16;

struct SockAddr {
    u8 len = 0;
    u8 family = 0;
    u16 port = 0;
    u32 addr = 0;
    u16 vport = 0;
};

SockAddr read_sockaddr(u8* base, u32 a)
{
    return {ps_read8(base, a), ps_read8(base, a + 1), ps_read16(base, a + 2), ps_read32(base, a + 4),
        ps_read16(base, a + 8)};
}

void write_sockaddr(u8* base, u32 a, const SockAddr& sa)
{
    ps_write8(base, a, sa.len);
    ps_write8(base, a + 1, sa.family);
    ps_write16(base, a + 2, sa.port);
    ps_write32(base, a + 4, sa.addr);
    ps_write16(base, a + 8, sa.vport);
    for (u32 i = 10; i < kSockaddrSize; i++) ps_write8(base, a + i, 0);
}

std::string sockaddr_text(const SockAddr& sa)
{
    std::string s = std::format("{}:{}", ip_text(sa.addr), sa.port);
    if (sa.vport) s += std::format(" vport {}", sa.vport);
    if (sa.family != SYS_NET_AF_INET) s += std::format(" (family {})", sa.family);
    return s;
}

// Where received bytes go: guest memory (checked before each write) or a host buffer (recvmsg's, scattered after).
struct Sink {
    PpuThread* t = nullptr;
    u32 guest = 0;               // guest memory at this address
    std::vector<u8>* host = nullptr; // or this buffer, grown as needed

    bool write(u32 offset, const u8* src, u32 n) const
    {
        if (!n) return true;
        if (host) {
            if (host->size() < usize{offset} + n) host->resize(usize{offset} + n);
            std::memcpy(host->data() + offset, src, n);
            return true;
        }
        if (!usable(*t, guest + offset, n, "recv: buf")) return false;
        std::memcpy(t->kernel.base() + guest + offset, src, n);
        return true;
    }
};


// ------------------------------------------------------------------------------------------------- sockets

struct Endpoint {
    u32 addr = 0; // host order: 0x7F000001 is 127.0.0.1
    u16 port = 0;
    u16 vport = 0; // P2P sockets
};

struct Datagram {
    Endpoint from;
    std::vector<u8> data;
};

// One direction of a loopback TCP connection.
struct Pipe {
    std::deque<u8> bytes;
    usize capacity = 0;       // the reader's SO_RCVBUF + the writer's SO_SNDBUF when it connected
    bool writer_done = false; // the writer shut down writing or closed: EOF once `bytes` is read
    bool reader_gone = false; // the reader closed: writing fails with EPIPE
    bool discard = false;     // the reader shut down reading: writes succeed and the bytes are dropped (BSD)
};

// The options a socket keeps (RPCS3 lv2_socket and the host socket behind it; BSD's defaults).
struct Options {
    s32 nbio = 0;
    s32 broadcast = 0;
    s32 reuseaddr = 0;
    s32 reuseport = 0;
    s32 keepalive = 0;
    u32 sndbuf = kDefaultSndBuf;
    u32 rcvbuf = kDefaultRcvBufDgram;
    s32 sndlowat = 2048; // BSD
    s32 rcvlowat = 1;
    u64 rcvtimeo_us = 0; // 0: none
    u64 sndtimeo_us = 0;
    s32 linger_onoff = 0, linger_secs = 0;
    s32 tcp_nodelay = 0;
    s32 tcp_maxseg = 1500; // RPCS3 so_tcp_maxseg
    s32 ip_hdrincl = 0, ip_tos = 0, ip_ttl = 64, ip_multicast_if = 0, ip_multicast_ttl = 1, ip_multicast_loop = 1;
    s32 ip_ttlchk = 0, ip_maxttl = 0, ip_dontfrag = 0;
    std::unordered_map<u64, std::vector<u8>> p2p; // RPCS3 lv2_socket_p2p sockopts: (level << 32 | name) -> value
};

struct Socket {
    s32 id = 0; // descriptor; 0 while a connection waits in its listener's queue
    s32 family = 0;
    s32 type = 0;
    s32 protocol = 0;
    bool closed = false; // closed: its waiters end with EINTR
    Options opt;
    std::vector<u32> groups; // multicast groups joined

    bool bound = false;
    Endpoint local;
    bool connected = false; // stream: a loopback connection; datagram: a default destination
    Endpoint peer;
    bool accepted = false; // the server side of a loopback connection (it shares its listener's port)
    bool listening = false;
    u32 backlog = 0;
    bool shut_rd = false;
    bool shut_wr = false;

    std::deque<Datagram> datagrams; // received, not read yet
    usize datagram_bytes = 0;
    std::shared_ptr<Pipe> in, out;              // stream connection
    std::deque<std::shared_ptr<Socket>> pending; // listener: connections not accepted yet
    bool wait_logged = false;
    u32 waiting = 0;   // threads blocked on it
    u64 aborts = 0;    // sys_net_abort_socket calls: a blocked call that sees it change ends with EINTR
};

bool is_p2p(const Socket& s) { return s.type == SYS_NET_SOCK_DGRAM_P2P || s.type == SYS_NET_SOCK_STREAM_P2P; }
bool reusable(const Socket& s) { return s.opt.reuseaddr || s.opt.reuseport; }

struct NetState {
    std::mutex m; // guards everything below
    std::array<std::shared_ptr<Socket>, kIdCount> table{};
    u32 highest = 0; // slots handed out in order so far (RPCS3 IdManager highest_index)
    u32 next_ephemeral = 0;
    u16 next_p2ps_vport = 0;
    WaitQueue waiters; // threads blocked in this module, woken (all) by every state change
    bool except_logged = false;
    std::array<u8, 512> test_param{};   // sys_net_set_test_param's block (the kernel keeps it)
    std::array<u8, 16> udpp2p_param{};  // sys_net_set_udpp2p_test_param's
    std::string resolv_conf;            // the resolver configuration (infoctl 52 sets it, 9 reads it)
};

NetState& net(Kernel& k)
{
    Kernel::Impl& s = k.impl();
    std::lock_guard lock(s.system_mutex);
    if (!s.net_state) s.net_state = std::make_shared<NetState>();
    return *static_cast<NetState*>(s.net_state.get());
}


std::shared_ptr<Socket> lookup(NetState& st, s32 id)
{
    if (id < static_cast<s32>(kIdBase) || id >= static_cast<s32>(kIdBase + kIdCount)) return nullptr;
    return st.table[static_cast<usize>(id) - kIdBase];
}

// RPCS3 idm::allocate_id for lv2_socket: the next slot until the range is used up, then the lowest free one. 0 when
// the table is full.
s32 insert(NetState& st, const std::shared_ptr<Socket>& sock)
{
    u32 index = kIdCount;
    if (st.highest < kIdCount) {
        index = st.highest++;
    } else {
        for (u32 i = 0; i < kIdCount && index == kIdCount; i++)
            if (!st.table[i]) index = i;
    }
    if (index == kIdCount) return 0;
    sock->id = static_cast<s32>(kIdBase + index);
    st.table[index] = sock;
    return sock->id;
}

void notify_all(NetState& st)
{
    for (Waiter* w : st.waiters.take_all(SYS_SYNC_FIFO)) wake(*w, CELL_OK);
}

std::optional<Clock::time_point> deadline_after(u64 us)
{
    if (!us) return std::nullopt;
    return deadline_after_us(us);
}

// Parks the calling thread until the next notify_all, or until `deadline` (none: no timeout). `lk` holds st.m on
// entry and on return. False when the deadline passed. The process exiting unwinds the thread (ThreadExit), as
// it does a blocked syscall.
bool sleep_until_notified(NetState& st, PpuThread& t, std::unique_lock<std::mutex>& lk,
    const std::optional<Clock::time_point>& deadline)
{
    u64 timeout_us = 0;
    if (deadline) {
        const auto now = Clock::now();
        if (now >= *deadline) return false;
        timeout_us = static_cast<u64>(
            std::max<s64>(1, std::chrono::ceil<std::chrono::microseconds>(*deadline - now).count()));
    }
    Waiter& w = t.waiter;
    prepare_wait(w);
    st.waiters.push(&w);
    const WaitStatus status = park(w, lk, timeout_us, {WaitKind::Net, 0}, [&](bool) {
        return st.waiters.remove(&w) ? CancelResult::Removed : CancelResult::Woken;
    });
    if (status == WaitStatus::Aborted) throw ThreadExit{};
    lk.lock();
    return status == WaitStatus::Woken;
}

// sleep_until_notified for a call blocked on these sockets: they count it as a waiter meanwhile (sys_net_abort_socket
// looks).
bool wait_on(NetState& st, PpuThread& t, std::unique_lock<std::mutex>& lk, std::initializer_list<Socket*> socks,
    const std::optional<Clock::time_point>& deadline)
{
    struct Count {
        std::initializer_list<Socket*> s;
        Count(std::initializer_list<Socket*> l) : s(l)
        {
            for (Socket* x : s) x->waiting++;
        }
        ~Count()
        {
            for (Socket* x : s) x->waiting--;
        }
    } count(socks);
    return sleep_until_notified(st, t, lk, deadline);
}

void log_wait_once(Socket& sock, std::string_view call, const std::optional<Clock::time_point>& deadline)
{
    if (deadline || sock.wait_logged) return;
    sock.wait_logged = true;
    PS_LOG_INFO(kNetChan, "{}(s {}): a blocking socket without a timeout; with no network it waits for loopback data, a "
                       "shutdown, a close, an abort or the process exit",
        call, sock.id);
}

bool port_used(const NetState& st, s32 type, u16 port)
{
    for (const auto& o : st.table)
        if (o && o->type == type && o->bound && !o->accepted && o->local.port == port) return true;
    return false;
}

u16 ephemeral_port(NetState& st, s32 type)
{
    for (u32 i = 0; i < kEphemeralCount; i++) {
        const u16 port = static_cast<u16>(kEphemeralFirst + st.next_ephemeral++ % kEphemeralCount);
        if (!port_used(st, type, port)) return port;
    }
    return 0;
}

// An unbound socket's implicit bind (BSD in_pcbbind with port 0): INADDR_ANY and an ephemeral port. False when no
// port is free.
bool bind_implicitly(NetState& st, Socket& sock)
{
    if (sock.bound) return true;
    const u16 port = ephemeral_port(st, sock.type);
    if (!port) return false;
    sock.bound = true;
    sock.local = {0, port, 0};
    return true;
}

// Closes a socket's side of everything (with st.m held): its peers see EOF and EPIPE, connections nobody accepted are
// closed with it, and its waiters end with EINTR once woken.
void close_socket(Socket& sock)
{
    sock.closed = true;
    if (sock.out) sock.out->writer_done = true;
    if (sock.in) {
        sock.in->reader_gone = true;
        sock.in->bytes.clear();
    }
    for (const auto& c : sock.pending) close_socket(*c);
    sock.pending.clear();
    sock.datagrams.clear();
    sock.datagram_bytes = 0;
}

// What getsockname reports (RPCS3 lv2_socket_*::getsockname): a native socket its address, 0.0.0.0:0 before it is
// bound; a P2P socket its address and virtual port (a stream one with the two swapped, as RPCS3 keeps them), all
// zero before it is bound; a raw socket all zero.
SockAddr local_name(const Socket& sock)
{
    const u8 len = kSockaddrSize, inet = SYS_NET_AF_INET;
    switch (sock.type) {
    case SYS_NET_SOCK_STREAM:
    case SYS_NET_SOCK_DGRAM: return {len, inet, sock.local.port, sock.local.addr, 0};
    case SYS_NET_SOCK_DGRAM_P2P:
        return sock.bound ? SockAddr{len, inet, sock.local.port, sock.local.addr, sock.local.vport} : SockAddr{};
    case SYS_NET_SOCK_STREAM_P2P:
        return sock.bound ? SockAddr{len, inet, sock.local.vport, sock.local.addr, sock.local.port} : SockAddr{};
    }
    return {};
}

// BSD soreadable / sowriteable for the sockets here. P2P stream sockets never connect and raw sockets never receive;
// RPCS3 reports neither ready.
bool readable(const Socket& sock)
{
    switch (sock.type) {
    case SYS_NET_SOCK_STREAM:
        if (sock.listening) return !sock.pending.empty();
        return sock.connected && (sock.shut_rd || !sock.in->bytes.empty() || sock.in->writer_done);
    case SYS_NET_SOCK_DGRAM:
    case SYS_NET_SOCK_DGRAM_P2P: return !sock.datagrams.empty() || sock.shut_rd;
    }
    return false;
}

bool writable(const Socket& sock)
{
    switch (sock.type) {
    case SYS_NET_SOCK_STREAM:
        return sock.connected && (sock.shut_wr || sock.out->reader_gone || sock.out->discard ||
                                     sock.out->bytes.size() < sock.out->capacity);
    case SYS_NET_SOCK_DGRAM:
    case SYS_NET_SOCK_DGRAM_P2P: return true;
    }
    return false;
}


// ================================================================================================ the kernel (lv2)
// Each bnet_* is one of lv2's network syscalls: a result, or -SYS_NET_E*.

// ------------------------------------------------------------------------------------------------- socket, close

// RPCS3 sys_net_bnet_socket: the type first; RPCS3 only logs another family. For stream and datagram sockets it
// asserts on another family or protocol (lv2_socket_native::create_socket), and a host's socket() refuses a TCP
// datagram or UDP stream socket with EPROTONOSUPPORT.
s32 bnet_socket(PpuThread& t, s32 family, s32 type, s32 protocol)
{
    const s32 r = [&]() -> s32 {
        switch (type) {
        case SYS_NET_SOCK_STREAM:
        case SYS_NET_SOCK_DGRAM:
        case SYS_NET_SOCK_RAW:
        case SYS_NET_SOCK_DGRAM_P2P:
        case SYS_NET_SOCK_STREAM_P2P: break;
        default: return -SYS_NET_EPROTONOSUPPORT;
        }
        if (type == SYS_NET_SOCK_STREAM || type == SYS_NET_SOCK_DGRAM) {
            const s32 own = type == SYS_NET_SOCK_STREAM ? SYS_NET_IPPROTO_TCP : SYS_NET_IPPROTO_UDP;
            if (family != SYS_NET_AF_INET) {
                PS_LOG_ERROR(kNetChan, "socket: family {} for a {} socket (RPCS3 asserts)", family, type_name(type));
                return -SYS_NET_EAFNOSUPPORT;
            }
            if (protocol != SYS_NET_IPPROTO_IP && protocol != own) {
                PS_LOG_ERROR(kNetChan, "socket: protocol {} for a {} socket", protocol, type_name(type));
                return -SYS_NET_EPROTONOSUPPORT;
            }
        } else if (family != SYS_NET_AF_INET) {
            PS_LOG_ERROR(kNetChan, "socket: family {} for a {} socket; created anyway, as RPCS3", family, type_name(type));
        }
        auto sock = std::make_shared<Socket>();
        sock->family = family;
        sock->type = type;
        sock->protocol = protocol;
        if (type == SYS_NET_SOCK_STREAM || type == SYS_NET_SOCK_STREAM_P2P) sock->opt.rcvbuf = kDefaultRcvBufStream;
        NetState& st = net(t.kernel);
        std::lock_guard lock(st.m);
        const s32 id = insert(st, sock);
        return id ? id : -SYS_NET_EMFILE; // RPCS3: "Can't allocate more than 1000 sockets"
    }();
    PS_LOG_INFO(kNetChan, "socket(family {}, {}, protocol {}): {}", family, type_name(type), protocol, result_text(r));
    return r;
}

// With st.m held: takes the socket out of the table and closes it (its blocked threads wake with EINTR).
void close_locked(NetState& st, std::shared_ptr<Socket> sock)
{
    st.table[static_cast<usize>(sock->id) - kIdBase] = nullptr;
    close_socket(*sock);
    notify_all(st);
}

// RPCS3 sys_net_bnet_close. A thread blocked on the socket wakes with EINTR (abort_socket).
s32 bnet_close(PpuThread& t, s32 s)
{
    const s32 r = [&]() -> s32 {
        NetState& st = net(t.kernel);
        std::lock_guard lock(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        close_locked(st, sock);
        return 0;
    }();
    PS_LOG_INFO(kNetChan, "socketclose(s {}): {}", s, result_text(r));
    return r;
}

// ------------------------------------------------------------------------------------------------- bind, listen

// BSD in_pcbbind for a stream or datagram socket, with the loopback interface the only one up.
s32 bind_native(NetState& st, Socket& sock, const SockAddr& sa)
{
    if (sock.bound) return -SYS_NET_EINVAL;
    if (sa.addr != 0 && sa.addr != kLoopback && !(sock.type == SYS_NET_SOCK_DGRAM && multicast(sa.addr))) {
        return -SYS_NET_EADDRNOTAVAIL;
    }
    u16 port = sa.port;
    if (!port) {
        port = ephemeral_port(st, sock.type);
        if (!port) return -SYS_NET_EADDRNOTAVAIL;
    } else {
        // Another socket on the port and an overlapping address, unless both allow reuse (SO_REUSEADDR or
        // SO_REUSEPORT, as RPCS3 treats them for P2P sockets).
        for (const auto& o : st.table) {
            if (!o || o.get() == &sock || o->type != sock.type || !o->bound || o->accepted || o->local.port != port) {
                continue;
            }
            if (o->local.addr && sa.addr && o->local.addr != sa.addr) continue;
            if (reusable(sock) && reusable(*o)) continue;
            return -SYS_NET_EADDRINUSE;
        }
    }
    sock.bound = true;
    sock.local = {sa.addr, port, 0};
    return 0;
}

// RPCS3 lv2_socket_p2p::bind and lv2_socket_p2ps::bind: a UDP port (SCE_NP_PORT normally) and a virtual port; a
// stream socket takes the virtual port from sin_port and the UDP port from sin_vport. A datagram socket needs a UDP
// port; a virtual port 0 picks a free one; a virtual port in use needs every socket bound to it to allow reuse
// (all_reusable).
s32 bind_p2p(NetState& st, Socket& sock, const SockAddr& sa)
{
    const bool stream = sock.type == SYS_NET_SOCK_STREAM_P2P;
    u16 port = stream ? sa.vport : sa.port;
    u16 vport = stream ? sa.port : sa.vport;
    if (!port) {
        if (!stream) return -SYS_NET_EINVAL;
        port = kNpPort;
    }
    const auto taken = [&](u16 v, bool reuse_ok) {
        for (const auto& o : st.table) {
            if (o && o.get() != &sock && o->type == sock.type && o->bound && o->local.port == port &&
                o->local.vport == v && !(reuse_ok && reusable(*o))) {
                return true;
            }
        }
        return false;
    };
    if (!vport) {
        if (stream) { // nt_p2p_port::get_port: a counter from 30000
            do {
                if (st.next_p2ps_vport < kP2pFirstVport) st.next_p2ps_vport = kP2pFirstVport;
                vport = st.next_p2ps_vport++;
            } while (taken(vport, false));
        } else { // the first free one from 30000
            vport = kP2pFirstVport;
            while (taken(vport, false)) vport++;
        }
    } else if (taken(vport, true)) {
        return -SYS_NET_EADDRINUSE;
    }
    sock.bound = true;
    sock.local = {sa.addr, port, vport};
    return 0;
}

// RPCS3 sys_net_bnet_bind: the address, the socket, then the family (AF_UNSPEC "presumably defaults to AF_INET").
s32 bnet_bind(PpuThread& t, s32 s, u32 addr, u32 addrlen)
{
    SockAddr sa{};
    const s32 r = [&]() -> s32 {
        if (!addr || addrlen < kSockaddrSize) return -SYS_NET_EINVAL;
        if (!usable(t, addr, kSockaddrSize, "bind: addr")) return -SYS_NET_EFAULT;
        sa = read_sockaddr(t.kernel.base(), addr);
        NetState& st = net(t.kernel);
        std::lock_guard lock(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        if (sa.family != SYS_NET_AF_INET && sa.family != SYS_NET_AF_UNSPEC) return -SYS_NET_EAFNOSUPPORT;
        switch (sock->type) {
        case SYS_NET_SOCK_STREAM:
        case SYS_NET_SOCK_DGRAM: return bind_native(st, *sock, sa);
        case SYS_NET_SOCK_DGRAM_P2P:
        case SYS_NET_SOCK_STREAM_P2P: return bind_p2p(st, *sock, sa);
        }
        return 0; // raw: RPCS3's stub accepts it and keeps no address
    }();
    PS_LOG_INFO(kNetChan, "bind(s {}, {}): {}", s, sockaddr_text(sa), result_text(r));
    return r;
}

// RPCS3 sys_net_bnet_listen: a positive backlog, then the socket. A datagram socket can't listen (BSD; RPCS3 aborts
// for a P2P one), an unbound stream socket gets an ephemeral port (BSD tcp_usr_listen), a connected one can't
// (BSD solisten). RPCS3's P2P stream socket listens without checks; its raw one ignores it.
s32 bnet_listen(PpuThread& t, s32 s, s32 backlog)
{
    const s32 r = [&]() -> s32 {
        if (backlog <= 0) return -SYS_NET_EINVAL;
        NetState& st = net(t.kernel);
        std::lock_guard lock(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        switch (sock->type) {
        case SYS_NET_SOCK_STREAM:
            if (sock->connected) return -SYS_NET_EINVAL;
            if (!bind_implicitly(st, *sock)) return -SYS_NET_EADDRNOTAVAIL;
            break;
        case SYS_NET_SOCK_STREAM_P2P: break;
        case SYS_NET_SOCK_DGRAM: return -SYS_NET_EOPNOTSUPP;
        case SYS_NET_SOCK_DGRAM_P2P:
            PS_LOG_ERROR(kNetChan, "listen on a DGRAM-P2P socket (RPCS3: fatal)");
            return -SYS_NET_EOPNOTSUPP;
        default: return 0;
        }
        sock->listening = true;
        sock->backlog = std::min(static_cast<u32>(backlog), kSomaxconn);
        return 0;
    }();
    PS_LOG_INFO(kNetChan, "listen(s {}, backlog {}): {}", s, backlog, result_text(r));
    return r;
}

// The peer getpeername reports (RPCS3 lv2_socket_*::getpeername): a connected stream or datagram socket's; ENOTCONN
// for the others, P2P ones included; a raw socket all zero (RPCS3's stub succeeds).
std::optional<SockAddr> peer_name(const Socket& sock, s32& error)
{
    switch (sock.type) {
    case SYS_NET_SOCK_STREAM:
    case SYS_NET_SOCK_DGRAM:
        if (sock.connected) return SockAddr{kSockaddrSize, SYS_NET_AF_INET, sock.peer.port, sock.peer.addr, 0};
        break;
    case SYS_NET_SOCK_RAW: return SockAddr{};
    }
    error = -SYS_NET_ENOTCONN;
    return std::nullopt;
}

// RPCS3 sys_net_bnet_getsockname and getpeername: both pointers, a length of at least 16, then the socket; the length
// comes back 16.
s32 bnet_sockname(PpuThread& t, s32 s, u32 addr, u32 paddrlen, bool peer)
{
    SockAddr sa{};
    const std::string_view call = peer ? "getpeername" : "getsockname";
    const s32 r = [&]() -> s32 {
        if (!addr || !paddrlen) return -SYS_NET_EINVAL;
        if (!usable(t, paddrlen, 4, std::format("{}: paddrlen", call))) return -SYS_NET_EFAULT;
        u8* base = t.kernel.base();
        if (ps_read32(base, paddrlen) < kSockaddrSize) return -SYS_NET_EINVAL;
        if (!usable(t, addr, kSockaddrSize, std::format("{}: addr", call))) return -SYS_NET_EFAULT;
        NetState& st = net(t.kernel);
        std::lock_guard lock(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        if (peer) {
            s32 error = 0;
            const auto p = peer_name(*sock, error);
            if (!p) return error;
            sa = *p;
        } else {
            sa = local_name(*sock);
        }
        write_sockaddr(base, addr, sa);
        ps_write32(base, paddrlen, kSockaddrSize);
        return 0;
    }();
    PS_LOG_DEBUG(kNetChan, "{}(s {}): {}, {}", call, s, result_text(r), sockaddr_text(sa));
    return r;
}

// ------------------------------------------------------------------------------------------------- socket options

// SO_RCVTIMEO / SO_SNDTIMEO: a sys_net_timeval (be s64 seconds and microseconds), kept in microseconds as RPCS3 keeps
// it. BSD refuses a negative or unnormalised one with EDOM.
s32 set_timeout(u8* base, u32 optval, u64& out)
{
    const s64 sec = static_cast<s64>(ps_read64(base, optval));
    const s64 usec = static_cast<s64>(ps_read64(base, optval + 8));
    if (sec < 0 || usec < 0 || usec >= 1000000) return -SYS_NET_EDOM;
    constexpr s64 kMaxSec = 1'000'000'000'000; // ~31700 years: no timeout in practice
    out = static_cast<u64>(std::min(sec, kMaxSec)) * 1000000u + static_cast<u64>(usec);
    return 0;
}

// IP_ADD_MEMBERSHIP / IP_DROP_MEMBERSHIP with a struct ip_mreq {group, interface}. Without a link only the loopback
// interface can join a group: with INADDR_ANY there is no route to choose an interface by (BSD: EADDRNOTAVAIL).
s32 membership(Socket& sock, bool add, u32 group, u32 iface)
{
    if (sock.type != SYS_NET_SOCK_DGRAM) return -SYS_NET_EOPNOTSUPP;
    if (!multicast(group)) return -SYS_NET_EINVAL;
    const auto it = std::find(sock.groups.begin(), sock.groups.end(), group);
    if (!add) {
        if (it == sock.groups.end()) return -SYS_NET_EADDRNOTAVAIL;
        sock.groups.erase(it);
        return 0;
    }
    if (iface != kLoopback) return -SYS_NET_EADDRNOTAVAIL;
    if (it != sock.groups.end()) return -SYS_NET_EADDRINUSE;
    sock.groups.push_back(group);
    return 0;
}

// The IP options kept as plain integers.
s32* ip_option(Options& o, s32 optname)
{
    switch (optname) {
    case SYS_NET_IP_HDRINCL: return &o.ip_hdrincl;
    case SYS_NET_IP_TOS: return &o.ip_tos;
    case SYS_NET_IP_TTL: return &o.ip_ttl;
    case SYS_NET_IP_MULTICAST_IF: return &o.ip_multicast_if;
    case SYS_NET_IP_MULTICAST_TTL: return &o.ip_multicast_ttl;
    case SYS_NET_IP_MULTICAST_LOOP: return &o.ip_multicast_loop;
    case SYS_NET_IP_TTLCHK: return &o.ip_ttlchk;
    case SYS_NET_IP_MAXTTL: return &o.ip_maxttl;
    case SYS_NET_IP_DONTFRAG: return &o.ip_dontfrag;
    }
    return nullptr;
}

// The options of lv2_socket_native::setsockopt (RPCS3), on the state kept here; an option RPCS3 hands to the host is
// checked as BSD checks it and kept for getsockopt. RPCS3's P2P sockets keep any option without checks (SO_NBIO and
// SO_BROADCAST take effect) and its raw ones take SO_NBIO; here both take SO_NBIO, SO_BROADCAST, the reuse options
// and the timeouts that way.
s32 set_option(u8* base, Socket& sock, s32 level, s32 optname, u32 optval, u32 optlen)
{
    const s32 value = static_cast<s32>(ps_read32(base, optval));
    if (is_p2p(sock) || sock.type == SYS_NET_SOCK_RAW) {
        if (is_p2p(sock)) {
            sock.opt.p2p[static_cast<u64>(static_cast<u32>(level)) << 32 | static_cast<u32>(optname)] =
                std::vector<u8>(base + optval, base + optval + std::min<u32>(optlen, 128));
        }
        if (level != SYS_NET_SOL_SOCKET) return 0;
        switch (optname) {
        case SYS_NET_SO_NBIO: sock.opt.nbio = value; break;
        case SYS_NET_SO_BROADCAST: sock.opt.broadcast = value; break;
        case SYS_NET_SO_REUSEADDR: sock.opt.reuseaddr = value; break;
        case SYS_NET_SO_REUSEPORT: sock.opt.reuseport = value; break;
        case SYS_NET_SO_RCVTIMEO:
        case SYS_NET_SO_SNDTIMEO:
            if (optlen >= 16) {
                u64 us = 0;
                if (set_timeout(base, optval, us) == 0) {
                    (optname == SYS_NET_SO_RCVTIMEO ? sock.opt.rcvtimeo_us : sock.opt.sndtimeo_us) = us;
                }
            }
            break;
        default: break;
        }
        return 0;
    }
    switch (level) {
    case SYS_NET_SOL_SOCKET:
        switch (optname) {
        case SYS_NET_SO_NBIO: sock.opt.nbio = value; return 0;
        case SYS_NET_SO_KEEPALIVE: sock.opt.keepalive = value != 0; return 0;
        case SYS_NET_SO_SNDBUF:
        case SYS_NET_SO_RCVBUF:
            if (value < 1) return -SYS_NET_EINVAL;
            (optname == SYS_NET_SO_SNDBUF ? sock.opt.sndbuf : sock.opt.rcvbuf) = static_cast<u32>(value);
            return 0;
        case SYS_NET_SO_SNDLOWAT:
        case SYS_NET_SO_RCVLOWAT:
            if (value < 1) return -SYS_NET_EINVAL;
            (optname == SYS_NET_SO_SNDLOWAT ? sock.opt.sndlowat : sock.opt.rcvlowat) = value;
            return 0;
        case SYS_NET_SO_BROADCAST: sock.opt.broadcast = value != 0; return 0;
        case SYS_NET_SO_REUSEADDR: sock.opt.reuseaddr = value != 0; return 0;
        case SYS_NET_SO_REUSEPORT: sock.opt.reuseport = value != 0; return 0;
        case SYS_NET_SO_SNDTIMEO:
        case SYS_NET_SO_RCVTIMEO:
            if (optlen < 16) return -SYS_NET_EINVAL;
            return set_timeout(base, optval, optname == SYS_NET_SO_SNDTIMEO ? sock.opt.sndtimeo_us : sock.opt.rcvtimeo_us);
        case SYS_NET_SO_LINGER:
            if (optlen < 8) return -SYS_NET_EINVAL;
            sock.opt.linger_onoff = value;
            sock.opt.linger_secs = static_cast<s32>(ps_read32(base, optval + 4));
            return 0;
        case SYS_NET_SO_USECRYPTO:
        case SYS_NET_SO_USESIGNATURE:
            PS_LOG_WARN(kNetChan, "setsockopt(s {}): option {:#x} accepted without effect (RPCS3's stub)", sock.id, optname);
            return 0;
        }
        PS_LOG_ERROR(kNetChan, "setsockopt(s {}, SOL_SOCKET): unknown option {:#x}", sock.id, optname);
        return -SYS_NET_EINVAL;
    case SYS_NET_IPPROTO_TCP:
        if (optname == SYS_NET_TCP_MAXSEG) { // RPCS3: kept, no effect
            sock.opt.tcp_maxseg = value;
            return 0;
        }
        if (optname == SYS_NET_TCP_NODELAY) {
            if (sock.type != SYS_NET_SOCK_STREAM) return -SYS_NET_EINVAL;
            sock.opt.tcp_nodelay = value != 0;
            return 0;
        }
        PS_LOG_ERROR(kNetChan, "setsockopt(s {}, IPPROTO_TCP): unknown option {:#x}", sock.id, optname);
        return -SYS_NET_EINVAL;
    case SYS_NET_IPPROTO_IP:
        if (optname == SYS_NET_IP_ADD_MEMBERSHIP || optname == SYS_NET_IP_DROP_MEMBERSHIP) {
            if (optlen < 8) return -SYS_NET_EINVAL;
            return membership(sock, optname == SYS_NET_IP_ADD_MEMBERSHIP, ps_read32(base, optval),
                ps_read32(base, optval + 4));
        }
        if (s32* v = ip_option(sock.opt, optname)) {
            *v = value;
            return 0;
        }
        PS_LOG_ERROR(kNetChan, "setsockopt(s {}, IPPROTO_IP): unknown option {:#x}", sock.id, optname);
        return -SYS_NET_EINVAL;
    }
    PS_LOG_ERROR(kNetChan, "setsockopt(s {}): unknown level {:#x}", sock.id, level);
    return -SYS_NET_EINVAL;
}

// RPCS3 sys_net_bnet_setsockopt: a length of at least 4 first, then the socket.
s32 bnet_setsockopt(PpuThread& t, s32 s, s32 level, s32 optname, u32 optval, u32 optlen)
{
    u32 value = 0;
    const s32 r = [&]() -> s32 {
        if (optlen < 4) return -SYS_NET_EINVAL;
        if (!usable(t, optval, optlen, "setsockopt: optval")) return -SYS_NET_EFAULT;
        u8* base = t.kernel.base();
        value = ps_read32(base, optval);
        NetState& st = net(t.kernel);
        std::lock_guard lock(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        return set_option(base, *sock, level, optname, optval, optlen);
    }();
    PS_LOG_INFO(kNetChan, "setsockopt(s {}, level {:#x}, option {:#x}, value {:#x}, optlen {}): {}", s, level, optname,
        value, optlen, result_text(r));
    return r;
}

// lv2_socket_native::getsockopt (RPCS3): the options setsockopt keeps, BSD's defaults for the rest; SO_ERROR is 0 (no
// asynchronous error happens offline), SO_TYPE the socket type (BSD). An int comes back in 4 bytes, a timeval in 16, a
// linger in 8. A P2P socket answers what was set (RPCS3 lv2_socket_p2p::getsockopt), nothing (length 0) otherwise; a
// raw socket nothing.
s32 get_option(const Socket& sock, s32 level, s32 optname, u32 len, std::vector<u8>& out)
{
    const auto put32 = [&](s32 v) {
        out.resize(4);
        ps_write32(out.data(), 0, static_cast<u32>(v));
        return 0;
    };
    if (is_p2p(sock)) {
        const auto it = sock.opt.p2p.find(static_cast<u64>(static_cast<u32>(level)) << 32 | static_cast<u32>(optname));
        if (it == sock.opt.p2p.end()) {
            PS_LOG_ERROR(kNetChan, "getsockopt(s {}): option {:#x}/{:#x} not set on a P2P socket (RPCS3: nothing)", sock.id,
                level, optname);
            out.clear();
            return 0;
        }
        out = it->second;
        return 0;
    }
    if (sock.type == SYS_NET_SOCK_RAW) {
        out.clear();
        return 0;
    }
    const Options& o = sock.opt;
    switch (level) {
    case SYS_NET_SOL_SOCKET:
        switch (optname) {
        case SYS_NET_SO_NBIO: return put32(o.nbio);
        case SYS_NET_SO_ERROR: return put32(0);
        case SYS_NET_SO_TYPE: return put32(sock.type);
        case SYS_NET_SO_KEEPALIVE: return put32(o.keepalive ? SYS_NET_SO_KEEPALIVE : 0);
        case SYS_NET_SO_SNDBUF: return put32(static_cast<s32>(o.sndbuf));
        case SYS_NET_SO_RCVBUF: return put32(static_cast<s32>(o.rcvbuf));
        case SYS_NET_SO_SNDLOWAT: return put32(o.sndlowat);
        case SYS_NET_SO_RCVLOWAT: return put32(o.rcvlowat);
        case SYS_NET_SO_BROADCAST: return put32(o.broadcast ? SYS_NET_SO_BROADCAST : 0);
        case SYS_NET_SO_REUSEADDR: return put32(o.reuseaddr ? SYS_NET_SO_REUSEADDR : 0);
        case SYS_NET_SO_REUSEPORT: return put32(o.reuseport ? SYS_NET_SO_REUSEPORT : 0);
        case SYS_NET_SO_SNDTIMEO:
        case SYS_NET_SO_RCVTIMEO: {
            if (len < 16) return -SYS_NET_EINVAL;
            const u64 us = optname == SYS_NET_SO_SNDTIMEO ? o.sndtimeo_us : o.rcvtimeo_us;
            out.resize(16);
            ps_write64(out.data(), 0, us / 1000000);
            ps_write64(out.data(), 8, us % 1000000);
            return 0;
        }
        case SYS_NET_SO_LINGER:
            if (len < 8) return -SYS_NET_EINVAL;
            out.resize(8);
            ps_write32(out.data(), 0, static_cast<u32>(o.linger_onoff));
            ps_write32(out.data(), 4, static_cast<u32>(o.linger_secs));
            return 0;
        }
        PS_LOG_ERROR(kNetChan, "getsockopt(s {}, SOL_SOCKET): unknown option {:#x}", sock.id, optname);
        return -SYS_NET_EINVAL;
    case SYS_NET_IPPROTO_TCP:
        if (optname == SYS_NET_TCP_MAXSEG) return put32(o.tcp_maxseg);
        if (optname == SYS_NET_TCP_NODELAY) {
            if (sock.type != SYS_NET_SOCK_STREAM) return -SYS_NET_EINVAL;
            return put32(o.tcp_nodelay);
        }
        PS_LOG_ERROR(kNetChan, "getsockopt(s {}, IPPROTO_TCP): unknown option {:#x}", sock.id, optname);
        return -SYS_NET_EINVAL;
    case SYS_NET_IPPROTO_IP:
        if (optname == SYS_NET_IP_ADD_MEMBERSHIP || optname == SYS_NET_IP_DROP_MEMBERSHIP) return -SYS_NET_EINVAL;
        if (const s32* v = ip_option(const_cast<Options&>(o), optname)) return put32(*v);
        PS_LOG_ERROR(kNetChan, "getsockopt(s {}, IPPROTO_IP): unknown option {:#x}", sock.id, optname);
        return -SYS_NET_EINVAL;
    }
    PS_LOG_ERROR(kNetChan, "getsockopt(s {}): unknown level {:#x}", sock.id, level);
    return -SYS_NET_EINVAL;
}

// RPCS3 sys_net_bnet_getsockopt: both pointers and a length other than 0, the socket, a length of at least 4, then the
// option; the length comes back as the option's.
s32 bnet_getsockopt(PpuThread& t, s32 s, s32 level, s32 optname, u32 optval, u32 optlen)
{
    const s32 r = [&]() -> s32 {
        if (!optval || !optlen) return -SYS_NET_EINVAL;
        if (!usable(t, optlen, 4, "getsockopt: optlen")) return -SYS_NET_EFAULT;
        u8* base = t.kernel.base();
        const u32 len = ps_read32(base, optlen);
        if (!len) return -SYS_NET_EINVAL;
        NetState& st = net(t.kernel);
        std::lock_guard lock(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        if (len < 4) return -SYS_NET_EINVAL;
        std::vector<u8> out;
        if (const s32 e = get_option(*sock, level, optname, len, out); e < 0) return e;
        if (!usable(t, optval, out.size(), "getsockopt: optval")) return -SYS_NET_EFAULT;
        std::memcpy(base + optval, out.data(), out.size());
        ps_write32(base, optlen, static_cast<u32>(out.size()));
        return 0;
    }();
    PS_LOG_DEBUG(kNetChan, "getsockopt(s {}, level {:#x}, option {:#x}): {}", s, level, optname, result_text(r));
    return r;
}

// ------------------------------------------------------------------------------------------------- shutdown

// RPCS3 sys_net_bnet_shutdown: `how` first, then the socket. A socket that isn't connected answers ENOTCONN (BSD
// soshutdown; the host behind RPCS3 does the same). RPCS3's P2P sockets treat it as a TODO and its raw ones as a stub.
s32 bnet_shutdown(PpuThread& t, s32 s, s32 how)
{
    const s32 r = [&]() -> s32 {
        if (how < SYS_NET_SHUT_RD || how > SYS_NET_SHUT_RDWR) return -SYS_NET_EINVAL;
        NetState& st = net(t.kernel);
        std::lock_guard lock(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        if (sock->type != SYS_NET_SOCK_STREAM && sock->type != SYS_NET_SOCK_DGRAM) return 0;
        if (!sock->connected) return -SYS_NET_ENOTCONN;
        if (how != SYS_NET_SHUT_WR) { // BSD sorflush: what is queued goes, what comes later is dropped
            sock->shut_rd = true;
            sock->datagrams.clear();
            sock->datagram_bytes = 0;
            if (sock->in) {
                sock->in->bytes.clear();
                sock->in->discard = true;
            }
        }
        if (how != SYS_NET_SHUT_RD) {
            sock->shut_wr = true;
            if (sock->out) sock->out->writer_done = true;
        }
        notify_all(st);
        return 0;
    }();
    PS_LOG_INFO(kNetChan, "shutdown(s {}, how {}): {}", s, how, result_text(r));
    return r;
}

// ------------------------------------------------------------------------------------------------- connect, accept

// The listening stream socket a loopback connection to `dest` reaches: bound to its port, on its address or
// INADDR_ANY (an exact address wins, as in BSD's lookup).
Socket* find_listener(NetState& st, const Endpoint& dest)
{
    Socket* wild = nullptr;
    for (const auto& o : st.table) {
        if (!o || o->type != SYS_NET_SOCK_STREAM || !o->listening || o->local.port != dest.port) continue;
        if (o->local.addr == dest.addr) return o.get();
        if (!o->local.addr) wild = o.get();
    }
    return wild;
}

// A TCP connect: RPCS3's offline check first (lv2_socket_native::connect), then BSD's (a listening socket can't, a
// connected one is connected, port 0 is no destination). A loopback connection completes at once; the server's side
// waits in the listener's queue with the listener's options (BSD sonewconn).
s32 connect_stream(NetState& st, Socket& sock, const SockAddr& sa)
{
    if (!loopback(sa.addr)) return -SYS_NET_EADDRNOTAVAIL;
    if (sock.listening) return -SYS_NET_EOPNOTSUPP;
    if (sock.connected) return -SYS_NET_EISCONN;
    if (!sa.port) return -SYS_NET_EADDRNOTAVAIL;
    const Endpoint dest{normalized(sa.addr), sa.port, 0};
    Socket* lst = find_listener(st, dest);
    if (!lst || lst->pending.size() >= lst->backlog) return -SYS_NET_ECONNREFUSED;
    if (!bind_implicitly(st, sock)) return -SYS_NET_EADDRNOTAVAIL;
    if (!sock.local.addr) sock.local.addr = kLoopback; // BSD in_pcbconnect: the route's source address

    auto srv = std::make_shared<Socket>();
    srv->family = lst->family;
    srv->type = SYS_NET_SOCK_STREAM;
    srv->protocol = lst->protocol;
    srv->opt = lst->opt;
    srv->bound = true;
    srv->accepted = true;
    srv->local = {dest.addr, lst->local.port, 0};
    srv->connected = true;
    srv->peer = sock.local;
    auto up = std::make_shared<Pipe>();   // client to server
    auto down = std::make_shared<Pipe>(); // server to client
    up->capacity = usize{srv->opt.rcvbuf} + sock.opt.sndbuf;
    down->capacity = usize{sock.opt.rcvbuf} + srv->opt.sndbuf;
    sock.out = up;
    sock.in = down;
    srv->in = up;
    srv->out = down;
    sock.connected = true;
    sock.peer = dest;
    lst->pending.push_back(std::move(srv));
    notify_all(st);
    return 0;
}

// A UDP connect sets the default destination (no traffic): RPCS3's offline check, then BSD udp_connect (a connected
// socket answers EISCONN, port 0 is no destination); the socket is bound implicitly, to the route's source address.
s32 connect_dgram(NetState& st, Socket& sock, const SockAddr& sa)
{
    if (!loopback(sa.addr)) return -SYS_NET_EADDRNOTAVAIL;
    if (sock.connected) return -SYS_NET_EISCONN;
    if (!sa.port) return -SYS_NET_EADDRNOTAVAIL;
    if (!bind_implicitly(st, sock)) return -SYS_NET_EADDRNOTAVAIL;
    if (!sock.local.addr) sock.local.addr = kLoopback;
    sock.connected = true;
    sock.peer = {normalized(sa.addr), sa.port, 0};
    return 0;
}

// RPCS3 sys_net_bnet_connect: the address and its family, then the socket.
s32 bnet_connect(PpuThread& t, s32 s, u32 addr, u32 addrlen)
{
    SockAddr sa{};
    const s32 r = [&]() -> s32 {
        if (!addr || addrlen < kSockaddrSize) return -SYS_NET_EINVAL;
        if (!usable(t, addr, kSockaddrSize, "connect: addr")) return -SYS_NET_EFAULT;
        sa = read_sockaddr(t.kernel.base(), addr);
        if (sa.family != SYS_NET_AF_INET) return -SYS_NET_EAFNOSUPPORT;
        NetState& st = net(t.kernel);
        std::lock_guard lock(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        switch (sock->type) {
        case SYS_NET_SOCK_STREAM: return connect_stream(st, *sock, sa);
        case SYS_NET_SOCK_DGRAM: return connect_dgram(st, *sock, sa);
        case SYS_NET_SOCK_STREAM_P2P:
            // RPCS3 lv2_socket_p2ps::connect: sin_port is the destination's virtual port; a socket that isn't closed
            // can't. Nothing answers a P2P handshake here.
            if (!sa.port) return -SYS_NET_EADDRNOTAVAIL;
            if (sock->listening) return -SYS_NET_EALREADY;
            return loopback(sa.addr) ? -SYS_NET_ECONNREFUSED : -SYS_NET_EADDRNOTAVAIL;
        }
        PS_LOG_ERROR(kNetChan, "connect on a {} socket (RPCS3: fatal)", type_name(sock->type));
        return -SYS_NET_EOPNOTSUPP;
    }();
    PS_LOG_INFO(kNetChan, "connect(s {}, {}): {}", s, sockaddr_text(sa), result_text(r));
    return r;
}

// RPCS3 sys_net_bnet_accept: addr and paddrlen both or neither, a length of at least 16, then the socket. The new
// socket inherits the listener's options; its descriptor is allocated when it is accepted (EMFILE leaves it queued).
s32 bnet_accept(PpuThread& t, s32 s, u32 addr, u32 paddrlen)
{
    SockAddr peer{};
    const s32 r = [&]() -> s32 {
        if ((addr != 0) != (paddrlen != 0)) return -SYS_NET_EINVAL;
        u8* base = t.kernel.base();
        if (paddrlen) {
            if (!usable(t, paddrlen, 4, "accept: paddrlen")) return -SYS_NET_EFAULT;
            if (ps_read32(base, paddrlen) < kSockaddrSize) return -SYS_NET_EINVAL;
            if (!usable(t, addr, kSockaddrSize, "accept: addr")) return -SYS_NET_EFAULT;
        }
        NetState& st = net(t.kernel);
        std::unique_lock lk(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        switch (sock->type) {
        case SYS_NET_SOCK_STREAM:
        case SYS_NET_SOCK_STREAM_P2P: break;
        case SYS_NET_SOCK_DGRAM: return -SYS_NET_EOPNOTSUPP;
        default:
            PS_LOG_ERROR(kNetChan, "accept on a {} socket (RPCS3: fatal)", type_name(sock->type));
            return -SYS_NET_EOPNOTSUPP;
        }
        if (!sock->listening) return -SYS_NET_EINVAL; // BSD: accept on a socket that doesn't listen
        const auto deadline = deadline_after(sock->opt.rcvtimeo_us);
        const u64 aborts = sock->aborts;
        for (;;) {
            if (sock->closed || sock->aborts != aborts) return -SYS_NET_EINTR;
            if (!sock->pending.empty()) {
                const auto conn = sock->pending.front();
                const s32 id = insert(st, conn);
                if (!id) return -SYS_NET_EMFILE;
                sock->pending.pop_front();
                peer = {kSockaddrSize, SYS_NET_AF_INET, conn->peer.port, conn->peer.addr, 0};
                if (addr) {
                    write_sockaddr(base, addr, peer);
                    ps_write32(base, paddrlen, kSockaddrSize);
                }
                return id;
            }
            if (sock->opt.nbio) return -SYS_NET_EWOULDBLOCK;
            log_wait_once(*sock, "accept", deadline);
            if (!wait_on(st, t, lk, {sock.get()}, deadline)) return -SYS_NET_EWOULDBLOCK;
        }
    }();
    if (r == -SYS_NET_EWOULDBLOCK) {
        PS_LOG_DEBUG(kNetChan, "accept(s {}): {}", s, result_text(r));
    } else {
        PS_LOG_INFO(kNetChan, "accept(s {}): {}{}", s, result_text(r), r >= 0 ? " from " + sockaddr_text(peer) : "");
    }
    return r;
}

// ------------------------------------------------------------------------------------------------- receiving

// What a receive gives back besides the bytes.
struct Received {
    Endpoint from;
    bool truncated = false; // a datagram longer than the buffer (recvmsg's MSG_TRUNC)
};

// recv on a TCP socket: what is queued, up to `len`; with MSG_WAITALL until `len` bytes (or EOF, a timeout, an
// error). EOF reads 0. Not connected (listening, or never connected): ENOTCONN (BSD soreceive).
s32 recv_stream(PpuThread& t, NetState& st, std::unique_lock<std::mutex>& lk, Socket& sock, const Sink& sink,
    u32 len, s32 flags, bool nonblock, const std::optional<Clock::time_point>& deadline)
{
    if (!sock.connected) return -SYS_NET_ENOTCONN;
    const bool peek = flags & SYS_NET_MSG_PEEK;
    const bool waitall = (flags & SYS_NET_MSG_WAITALL) && !peek;
    const u64 aborts = sock.aborts;
    u32 got = 0;
    const auto partial = [&](s32 error) { return got ? static_cast<s32>(got) : error; };
    std::vector<u8> chunk;
    for (;;) {
        if (sock.closed || sock.aborts != aborts) return partial(-SYS_NET_EINTR);
        if (sock.shut_rd) return static_cast<s32>(got);
        Pipe& in = *sock.in;
        const u32 n = static_cast<u32>(std::min<usize>(in.bytes.size(), len - got));
        if (n) {
            chunk.assign(in.bytes.begin(), in.bytes.begin() + n);
            if (!sink.write(got, chunk.data(), n)) return partial(-SYS_NET_EFAULT);
            if (!peek) {
                in.bytes.erase(in.bytes.begin(), in.bytes.begin() + n);
                notify_all(st); // room for the writer
            }
            got += n;
            if (!waitall || got == len) return static_cast<s32>(got);
        }
        if (in.writer_done) return static_cast<s32>(got);
        if (nonblock) return partial(-SYS_NET_EWOULDBLOCK);
        log_wait_once(sock, "recv", deadline);
        if (!wait_on(st, t, lk, {&sock}, deadline)) return partial(-SYS_NET_EWOULDBLOCK);
    }
}

// recvfrom on a UDP or P2P datagram socket: the next datagram, cut to `len` (the rest is dropped unless MSG_PEEK).
s32 recv_dgram(PpuThread& t, NetState& st, std::unique_lock<std::mutex>& lk, Socket& sock, const Sink& sink,
    u32 len, s32 flags, bool nonblock, const std::optional<Clock::time_point>& deadline, Received& got)
{
    const u64 aborts = sock.aborts;
    for (;;) {
        if (sock.closed || sock.aborts != aborts) return -SYS_NET_EINTR;
        if (!sock.datagrams.empty()) {
            const Datagram& d = sock.datagrams.front();
            const u32 n = static_cast<u32>(std::min<usize>(len, d.data.size()));
            if (!sink.write(0, d.data.data(), n)) return -SYS_NET_EFAULT;
            got.from = d.from;
            got.truncated = d.data.size() > len;
            if (!(flags & SYS_NET_MSG_PEEK)) {
                sock.datagram_bytes -= d.data.size();
                sock.datagrams.pop_front();
            }
            return static_cast<s32>(n);
        }
        if (sock.shut_rd) return 0;
        if (nonblock) return -SYS_NET_EWOULDBLOCK;
        log_wait_once(sock, "recvfrom", deadline);
        if (!wait_on(st, t, lk, {&sock}, deadline)) return -SYS_NET_EWOULDBLOCK;
    }
}

// A socket nothing can reach (raw, a listening P2P stream socket): EWOULDBLOCK, or a wait for the timeout or close.
s32 recv_nothing(PpuThread& t, NetState& st, std::unique_lock<std::mutex>& lk, Socket& sock, bool nonblock,
    const std::optional<Clock::time_point>& deadline)
{
    const u64 aborts = sock.aborts;
    for (;;) {
        if (sock.closed || sock.aborts != aborts) return -SYS_NET_EINTR;
        if (nonblock) return -SYS_NET_EWOULDBLOCK;
        log_wait_once(sock, "recvfrom", deadline);
        if (!wait_on(st, t, lk, {&sock}, deadline)) return -SYS_NET_EWOULDBLOCK;
    }
}

// A receive on socket `s` into `sink`, up to `len` bytes; the source in `got`. RPCS3 sys_net_bnet_recvfrom's checks
// past the buffer's are the caller's.
s32 receive(PpuThread& t, s32 s, const Sink& sink, u32 len, s32 flags, Received& got, s32& type)
{
    NetState& st = net(t.kernel);
    std::unique_lock lk(st.m);
    const auto sock = lookup(st, s);
    if (!sock) return -SYS_NET_EBADF;
    type = sock->type;
    const bool nonblock = sock->opt.nbio || (flags & SYS_NET_MSG_DONTWAIT);
    const auto deadline = deadline_after(sock->opt.rcvtimeo_us);
    switch (sock->type) {
    case SYS_NET_SOCK_STREAM: return recv_stream(t, st, lk, *sock, sink, len, flags, nonblock, deadline);
    case SYS_NET_SOCK_DGRAM:
    case SYS_NET_SOCK_DGRAM_P2P: return recv_dgram(t, st, lk, *sock, sink, len, flags, nonblock, deadline, got);
    case SYS_NET_SOCK_STREAM_P2P:
        if (!sock->listening) return -SYS_NET_ENOTCONN; // RPCS3 lv2_socket_p2ps::recvfrom on a closed stream
        [[fallthrough]];
    default: return recv_nothing(t, st, lk, *sock, nonblock, deadline);
    }
}

// The source address a receive reports (RPCS3 sys_net_bnet_recvfrom): a datagram's source, a zero port and address
// for stream data, all zero for a result of 0.
SockAddr source_of(s32 n, s32 type, const Received& got)
{
    if (n <= 0) return {};
    const bool dgram = type == SYS_NET_SOCK_DGRAM || type == SYS_NET_SOCK_DGRAM_P2P;
    return {kSockaddrSize, SYS_NET_AF_INET, dgram ? got.from.port : u16{0}, dgram ? got.from.addr : 0,
        type == SYS_NET_SOCK_DGRAM_P2P ? got.from.vport : u16{0}};
}

// RPCS3 sys_net_bnet_recvfrom: a buffer, a length, addr and paddrlen both or neither, known flags, then the socket. A
// timeout (SO_RCVTIMEO) ends with EWOULDBLOCK. The address comes back as source_of; the length 16.
s32 bnet_recvfrom(PpuThread& t, s32 s, u32 buf, u32 len, s32 flags, u32 addr, u32 paddrlen)
{
    const s32 r = [&]() -> s32 {
        if (!buf || !len || (addr != 0) != (paddrlen != 0)) return -SYS_NET_EINVAL;
        if (flags & ~kRecvFlags) {
            PS_LOG_ERROR(kNetChan, "recvfrom(s {}): unknown flags {:#x} (RPCS3 aborts)", s, flags);
            return -SYS_NET_EINVAL;
        }
        if (addr && (!usable(t, paddrlen, 4, "recvfrom: paddrlen") ||
                        !usable(t, addr, kSockaddrSize, "recvfrom: addr"))) {
            return -SYS_NET_EFAULT;
        }
        Received got;
        s32 type = 0;
        const s32 n = receive(t, s, Sink{&t, buf, nullptr}, len, flags, got, type);
        if (n >= 0 && addr) {
            u8* base = t.kernel.base();
            write_sockaddr(base, addr, source_of(n, type, got));
            ps_write32(base, paddrlen, kSockaddrSize);
        }
        return n;
    }();
    PS_LOG_DEBUG(kNetChan, "recvfrom(s {}, len {:#x}, flags {:#x}): {}", s, len, flags, result_text(r));
    return r;
}

// sys_net_msghdr (48 bytes; RPCS3 lv2/sys_net.h, libnet zeroes the high words of the pointers): msg_name at +4,
// msg_namelen +8, msg_iov +20, msg_iovlen +24, msg_control +36, msg_controllen +40, msg_flags +44. sys_net_iovec (16
// bytes): iov_base +4, iov_len a 64-bit +8.
constexpr u32 kMsghdrSize = 48, kIovecSize = 16;

struct Iovec {
    u32 base = 0, len = 0;
};

// The message's iovecs (checked: each must be guest memory, and a length past 2 GB is no length; BSD EINVAL).
std::optional<std::vector<Iovec>> read_iovecs(PpuThread& t, u32 msg, s32& error)
{
    u8* base = t.kernel.base();
    const u32 iov = ps_read32(base, msg + 20);
    const s32 iovlen = static_cast<s32>(ps_read32(base, msg + 24));
    std::vector<Iovec> out;
    if (iovlen <= 0) return out;
    if (iovlen > 1024) { // BSD UIO_MAXIOV
        error = -SYS_NET_EINVAL;
        return std::nullopt;
    }
    if (!usable(t, iov, u64{kIovecSize} * static_cast<u32>(iovlen), "msghdr: msg_iov")) {
        error = -SYS_NET_EFAULT;
        return std::nullopt;
    }
    u64 total = 0;
    for (s32 i = 0; i < iovlen; i++) {
        const u32 at = iov + kIovecSize * static_cast<u32>(i);
        const u64 len = ps_read64(base, at + 8);
        total += len;
        if (len > 0x7FFFFFFF || total > 0x7FFFFFFF) {
            error = -SYS_NET_EINVAL;
            return std::nullopt;
        }
        out.push_back({ps_read32(base, at + 4), static_cast<u32>(len)});
        if (!usable(t, out.back().base, out.back().len, "msghdr: iov_base")) {
            error = -SYS_NET_EFAULT;
            return std::nullopt;
        }
    }
    return out;
}

// sys_net_bnet_recvmsg: recvfrom into the message's buffers, BSD's way (RPCS3: a TODO answering 0). msg_name gets the
// source (as recvfrom's address, cut to msg_namelen, which comes back 16), msg_controllen 0 (no control data),
// msg_flags MSG_TRUNC for a datagram longer than the buffers.
s32 bnet_recvmsg(PpuThread& t, s32 s, u32 msg, s32 flags)
{
    const s32 r = [&]() -> s32 {
        if (!msg) return -SYS_NET_EINVAL;
        if (flags & ~kRecvFlags) {
            PS_LOG_ERROR(kNetChan, "recvmsg(s {}): unknown flags {:#x}", s, flags);
            return -SYS_NET_EINVAL;
        }
        if (!usable(t, msg, kMsghdrSize, "recvmsg: msg")) return -SYS_NET_EFAULT;
        s32 error = 0;
        const auto iovs = read_iovecs(t, msg, error);
        if (!iovs) return error;
        u8* base = t.kernel.base();
        const u32 name = ps_read32(base, msg + 4), namelen = ps_read32(base, msg + 8);
        if (name && !usable(t, name, std::min(namelen, kSockaddrSize), "recvmsg: msg_name")) return -SYS_NET_EFAULT;
        u64 total = 0;
        for (const Iovec& v : *iovs) total += v.len;
        std::vector<u8> data;
        Received got;
        s32 type = 0;
        const s32 n = receive(t, s, Sink{&t, 0, &data}, static_cast<u32>(total), flags, got, type);
        if (n < 0) return n;
        u32 done = 0;
        for (const Iovec& v : *iovs) {
            const u32 k = std::min(v.len, static_cast<u32>(n) - done);
            std::memcpy(base + v.base, data.data() + done, k);
            done += k;
        }
        if (name) {
            std::array<u8, kSockaddrSize> sa{};
            const SockAddr src = source_of(n, type, got);
            sa[0] = src.len;
            sa[1] = src.family;
            ps_write16(sa.data(), 2, src.port);
            ps_write32(sa.data(), 4, src.addr);
            ps_write16(sa.data(), 8, src.vport);
            std::memcpy(base + name, sa.data(), std::min(namelen, kSockaddrSize));
            ps_write32(base, msg + 8, std::min(namelen, kSockaddrSize));
        }
        ps_write32(base, msg + 40, 0);
        ps_write32(base, msg + 44, got.truncated ? SYS_NET_MSG_TRUNC : 0);
        return n;
    }();
    PS_LOG_DEBUG(kNetChan, "recvmsg(s {}, flags {:#x}): {}", s, flags, result_text(r));
    return r;
}

// ------------------------------------------------------------------------------------------------- sending

// A loopback datagram to the socket of the sender's type bound to `dest` (its port, its virtual port for P2P, its
// address or INADDR_ANY; an exact address wins; a connected socket takes only its peer's). Nobody bound there, a
// receiver that shut down reading or a full SO_RCVBUF drop it, as BSD does (the sender still succeeds).
void deliver(NetState& st, const Socket& from, const Endpoint& dest, const u8* data, u32 len)
{
    Endpoint src = from.local;
    if (!src.addr) src.addr = kLoopback;
    Socket* to = nullptr;
    for (const auto& o : st.table) {
        if (!o || o->type != from.type || !o->bound || o->local.port != dest.port) continue;
        if (from.type == SYS_NET_SOCK_DGRAM_P2P && o->local.vport != dest.vport) continue;
        if (o->local.addr && o->local.addr != dest.addr) continue;
        if (o->connected && (o->peer.addr != src.addr || o->peer.port != src.port)) continue;
        if (!to || (o->local.addr && !to->local.addr)) to = o.get();
    }
    std::string_view dropped;
    if (!to) dropped = "no socket is bound there";
    else if (to->shut_rd) dropped = "the receiver shut down reading";
    else if (to->datagram_bytes + len > to->opt.rcvbuf) dropped = "the receiver's buffer is full";
    if (!dropped.empty()) {
        PS_LOG_DEBUG(kNetChan, "loopback datagram of {} bytes to {}:{} dropped: {}", len, ip_text(dest.addr), dest.port,
            dropped);
        return;
    }
    to->datagrams.push_back({src, std::vector<u8>(data, data + len)});
    to->datagram_bytes += len;
}

// send on a TCP socket: RPCS3's offline check of a given address first; then into the connection, as much as its
// buffers take, waiting for room on a blocking socket (SO_SNDTIMEO ends the wait with what was sent, or EWOULDBLOCK).
s32 send_stream(PpuThread& t, NetState& st, std::unique_lock<std::mutex>& lk, Socket& sock, const u8* src, u32 len,
    bool nonblock, const std::optional<SockAddr>& to)
{
    if (to && !loopback(to->addr)) return -SYS_NET_EADDRNOTAVAIL;
    if (!sock.connected) return -SYS_NET_ENOTCONN;
    const auto deadline = deadline_after(sock.opt.sndtimeo_us);
    const u64 aborts = sock.aborts;
    u32 sent = 0;
    const auto partial = [&](s32 error) { return sent ? static_cast<s32>(sent) : error; };
    for (;;) {
        if (sock.closed || sock.aborts != aborts) return partial(-SYS_NET_EINTR);
        Pipe& out = *sock.out;
        if (sock.shut_wr || out.reader_gone) return partial(-SYS_NET_EPIPE);
        if (sent == len || out.discard) return static_cast<s32>(len);
        if (out.bytes.size() < out.capacity) {
            const u32 n = static_cast<u32>(std::min<usize>(out.capacity - out.bytes.size(), len - sent));
            out.bytes.insert(out.bytes.end(), src + sent, src + sent + n);
            sent += n;
            notify_all(st);
            continue;
        }
        if (nonblock) return partial(-SYS_NET_EWOULDBLOCK);
        if (!wait_on(st, t, lk, {&sock}, deadline)) return partial(-SYS_NET_EWOULDBLOCK);
    }
}

// sendto on a UDP socket: RPCS3's offline check, then BSD udp_output (an address and a connection exclude each other;
// a datagram fits SO_SNDBUF and IP; port 0 is no destination); an unbound sender is bound implicitly.
s32 send_dgram(NetState& st, Socket& sock, const u8* src, u32 len, const std::optional<SockAddr>& to)
{
    if (to && !loopback(to->addr)) return -SYS_NET_EADDRNOTAVAIL;
    if (to && sock.connected) return -SYS_NET_EISCONN;
    if (!to && !sock.connected) return -SYS_NET_EDESTADDRREQ;
    if (sock.shut_wr) return -SYS_NET_EPIPE;
    if (len > std::min(sock.opt.sndbuf, kUdpMaxPayload)) return -SYS_NET_EMSGSIZE;
    const Endpoint dest = to ? Endpoint{normalized(to->addr), to->port, 0} : sock.peer;
    if (!dest.port) return -SYS_NET_EADDRNOTAVAIL;
    if (!bind_implicitly(st, sock)) return -SYS_NET_EADDRNOTAVAIL;
    deliver(st, sock, dest, src, len);
    notify_all(st);
    return static_cast<s32>(len);
}

// sendto on a P2P datagram socket (RPCS3 lv2_socket_p2p::sendto, which asserts an address, a bound socket and a
// payload that fits; refuses a broadcast without SO_BROADCAST), then the offline check.
s32 send_p2p(NetState& st, Socket& sock, const u8* src, u32 len, const std::optional<SockAddr>& to)
{
    if (!to || !sock.bound || len > kP2pMaxPayload) {
        PS_LOG_ERROR(kNetChan, "sendto(s {}): {} (RPCS3 asserts)", sock.id,
            !to ? "no address" : !sock.bound ? "socket not bound" : "payload too large");
        return !to ? -SYS_NET_EDESTADDRREQ : !sock.bound ? -SYS_NET_EINVAL : -SYS_NET_EMSGSIZE;
    }
    if (to->addr == kInaddrBroadcast && !sock.opt.broadcast) return -SYS_NET_EACCES;
    if (!loopback(to->addr)) return -SYS_NET_EADDRNOTAVAIL;
    deliver(st, sock, {normalized(to->addr), to->port, to->vport}, src, len);
    notify_all(st);
    return static_cast<s32>(len);
}

// The bytes `src` (copied out of guest memory first) to socket `s`, to `to` when given.
s32 transmit(PpuThread& t, s32 s, const u8* src, u32 len, s32 flags, const std::optional<SockAddr>& to)
{
    NetState& st = net(t.kernel);
    std::unique_lock lk(st.m);
    const auto sock = lookup(st, s);
    if (!sock) return -SYS_NET_EBADF;
    const bool nonblock = sock->opt.nbio || (flags & SYS_NET_MSG_DONTWAIT);
    switch (sock->type) {
    case SYS_NET_SOCK_STREAM: return send_stream(t, st, lk, *sock, src, len, nonblock, to);
    case SYS_NET_SOCK_DGRAM: return send_dgram(st, *sock, src, len, to);
    case SYS_NET_SOCK_DGRAM_P2P: return send_p2p(st, *sock, src, len, to);
    case SYS_NET_SOCK_STREAM_P2P: return -SYS_NET_ENOTCONN; // RPCS3 lv2_socket_p2ps::sendto on a closed stream
    }
    // Raw: RPCS3's stub takes everything; nothing receives raw packets here.
    if (to && !loopback(to->addr)) return -SYS_NET_EADDRNOTAVAIL;
    return static_cast<s32>(len);
}

// The destination address of a send (sendto's, sendmsg's msg_name): at least 16 bytes, AF_INET.
s32 destination(PpuThread& t, u32 addr, u32 addrlen, std::optional<SockAddr>& to)
{
    if (addrlen < kSockaddrSize) return -SYS_NET_EINVAL;
    if (!usable(t, addr, kSockaddrSize, "sendto: addr")) return -SYS_NET_EFAULT;
    to = read_sockaddr(t.kernel.base(), addr);
    if (to->family != SYS_NET_AF_INET) return -SYS_NET_EAFNOSUPPORT;
    return 0;
}

// RPCS3 sys_net_bnet_sendto: known flags, an address of at least 16 bytes and AF_INET when given, the buffer (copied
// whole), then the socket.
s32 bnet_sendto(PpuThread& t, s32 s, u32 buf, u32 len, s32 flags, u32 addr, u32 addrlen)
{
    std::optional<SockAddr> to;
    const s32 r = [&]() -> s32 {
        if (flags & ~kSendFlags) {
            PS_LOG_ERROR(kNetChan, "sendto(s {}): unknown flags {:#x} (RPCS3 aborts)", s, flags);
            return -SYS_NET_EINVAL;
        }
        if (addr) {
            if (const s32 e = destination(t, addr, addrlen, to); e < 0) return e;
        }
        if (!usable(t, buf, len, "sendto: buf")) return -SYS_NET_EFAULT;
        const u8* src = t.kernel.base() + buf;
        const std::vector<u8> copy(src, src + len);
        return transmit(t, s, copy.data(), len, flags, to);
    }();
    PS_LOG_DEBUG(kNetChan, "sendto(s {}, len {:#x}, flags {:#x}{}): {}", s, len, flags,
        to ? ", to " + sockaddr_text(*to) : "", result_text(r));
    return r;
}

// sys_net_bnet_sendmsg (RPCS3 sends the gathered buffers as send() does): known flags, the message, then as sendto,
// with msg_name as the destination when there is one (BSD sosend). No control data is taken.
s32 bnet_sendmsg(PpuThread& t, s32 s, u32 msg, s32 flags)
{
    std::optional<SockAddr> to;
    const s32 r = [&]() -> s32 {
        if (flags & ~kSendFlags) {
            PS_LOG_ERROR(kNetChan, "sendmsg(s {}): unknown flags {:#x} (RPCS3 throws)", s, flags);
            return -SYS_NET_EINVAL;
        }
        if (!msg) return -SYS_NET_EINVAL;
        if (!usable(t, msg, kMsghdrSize, "sendmsg: msg")) return -SYS_NET_EFAULT;
        s32 error = 0;
        const auto iovs = read_iovecs(t, msg, error);
        if (!iovs) return error;
        u8* base = t.kernel.base();
        if (const u32 name = ps_read32(base, msg + 4)) {
            if (const s32 e = destination(t, name, ps_read32(base, msg + 8), to); e < 0) return e;
        }
        if (ps_read32(base, msg + 36) && ps_read32(base, msg + 40)) {
            PS_LOG_WARN(kNetChan, "sendmsg(s {}): control data ignored", s);
        }
        std::vector<u8> data;
        for (const Iovec& v : *iovs) data.insert(data.end(), base + v.base, base + v.base + v.len);
        return transmit(t, s, data.data(), static_cast<u32>(data.size()), flags, to);
    }();
    PS_LOG_DEBUG(kNetChan, "sendmsg(s {}, flags {:#x}{}): {}", s, flags, to ? ", to " + sockaddr_text(*to) : "",
        result_text(r));
    return r;
}

// ------------------------------------------------------------------------------------------------- select, poll

// sys_net_fd_set: 1024 bits in 32 big-endian words; descriptor d is bit d % 32 of word d / 32 (RPCS3).
constexpr u32 kFdSetSize = 128;
using FdSet = std::array<u32, 32>;

FdSet read_fdset(u8* base, u32 addr)
{
    FdSet set{};
    for (u32 i = 0; i < set.size(); i++) set[i] = ps_read32(base, addr + 4 * i);
    return set;
}

void write_fdset(u8* base, u32 addr, const FdSet& set)
{
    for (u32 i = 0; i < set.size(); i++) ps_write32(base, addr + 4 * i, set[i]);
}

bool has_fd(const FdSet& set, s32 fd) { return (set[static_cast<u32>(fd >> 5) & 31] >> (fd & 31)) & 1; }
void add_fd(FdSet& set, s32 fd) { set[static_cast<u32>(fd >> 5) & 31] |= 1u << (fd & 31); }

// A wait on several sockets (select, poll): woken by any change; a selected socket closed or aborted meanwhile ends it
// with EINTR.
struct MultiWait {
    std::vector<std::shared_ptr<Socket>> socks;
    std::vector<u64> aborts;

    void add(std::shared_ptr<Socket> s)
    {
        aborts.push_back(s->aborts);
        socks.push_back(std::move(s));
    }
    bool interrupted() const
    {
        for (usize i = 0; i < socks.size(); i++)
            if (socks[i]->closed || socks[i]->aborts != aborts[i]) return true;
        return false;
    }
    bool sleep(NetState& st, PpuThread& t, std::unique_lock<std::mutex>& lk,
        const std::optional<Clock::time_point>& deadline)
    {
        for (const auto& s : socks) s->waiting++;
        struct Undo {
            MultiWait& w;
            ~Undo()
            {
                for (const auto& s : w.socks) s->waiting--;
            }
        } undo{*this};
        return sleep_until_notified(st, t, lk, deadline);
    }
};

// RPCS3 sys_net_bnet_select: the timeout (tv_sec * 1000000 + tv_usec, unsigned; none: forever; 0: a poll), 0 < nfds
// <= 1024, a socket for every descriptor below nfds set in readfds or writefds (EBADF otherwise). The result counts the
// sockets with an event and replaces the sets; exceptfds comes back empty. A selected socket closed during the wait
// ends it with EINTR.
s32 bnet_select(PpuThread& t, s32 nfds, u32 readfds, u32 writefds, u32 exceptfds, u32 timeout)
{
    std::optional<u64> timeout_us;
    const s32 r = [&]() -> s32 {
        u8* base = t.kernel.base();
        if (timeout) {
            if (!usable(t, timeout, 16, "socketselect: timeout")) return -SYS_NET_EFAULT;
            timeout_us = ps_read64(base, timeout) * 1000000u + ps_read64(base, timeout + 8);
        }
        NetState& st = net(t.kernel);
        if (exceptfds) {
            std::lock_guard lock(st.m);
            if (!st.except_logged) PS_LOG_WARN(kNetChan, "socketselect: exceptfds is never set (as RPCS3)");
            st.except_logged = true;
        }
        if (nfds <= 0 || nfds > 1024) return -SYS_NET_EINVAL;
        FdSet rset{}, wset{};
        if (readfds) {
            if (!usable(t, readfds, kFdSetSize, "socketselect: readfds")) return -SYS_NET_EFAULT;
            rset = read_fdset(base, readfds);
        }
        if (writefds) {
            if (!usable(t, writefds, kFdSetSize, "socketselect: writefds")) return -SYS_NET_EFAULT;
            wset = read_fdset(base, writefds);
        }
        if (exceptfds && !usable(t, exceptfds, kFdSetSize, "socketselect: exceptfds")) return -SYS_NET_EFAULT;

        struct Selected {
            s32 fd;
            Socket* sock;
            bool read, write;
        };
        std::vector<Selected> selected;
        MultiWait wait;
        std::unique_lock lk(st.m);
        for (s32 fd = 0; fd < nfds; fd++) {
            const bool rd = readfds && has_fd(rset, fd), wr = writefds && has_fd(wset, fd);
            if (!rd && !wr) continue;
            auto sock = lookup(st, fd);
            if (!sock) return -SYS_NET_EBADF;
            selected.push_back({fd, sock.get(), rd, wr});
            wait.add(std::move(sock));
        }
        const bool poll_only = timeout_us && !*timeout_us;
        const auto deadline = timeout_us ? deadline_after(*timeout_us) : std::nullopt;
        bool timed_out = false;
        for (;;) {
            FdSet rout{}, wout{};
            s32 count = 0;
            for (const Selected& x : selected) {
                const bool rd = x.read && readable(*x.sock), wr = x.write && writable(*x.sock);
                if (rd) add_fd(rout, x.fd);
                if (wr) add_fd(wout, x.fd);
                if (rd || wr) count++;
            }
            if (count || poll_only || timed_out) {
                if (readfds) write_fdset(base, readfds, rout);
                if (writefds) write_fdset(base, writefds, wout);
                if (exceptfds) write_fdset(base, exceptfds, FdSet{});
                return count;
            }
            timed_out = !wait.sleep(st, t, lk, deadline);
            if (wait.interrupted()) return -SYS_NET_EINTR;
        }
    }();
    PS_LOG_DEBUG(kNetChan, "socketselect(nfds {}, timeout {}): {}", nfds,
        timeout_us ? std::format("{} us", *timeout_us) : std::string("none"), result_text(r));
    return r;
}

// RPCS3 sys_net_bnet_poll: nfds <= 0 answers 0 at once; ms 0 polls, a negative ms waits for good, else ms
// milliseconds. Each sys_net_pollfd {s32 fd, s16 events, s16 revents}: a negative fd is skipped, an unknown one gets
// POLLNVAL; POLLIN when readable and asked for, POLLOUT when writable and asked for. The result counts the entries
// with revents; the array comes back whole. A polled socket closed or aborted during the wait ends it with EINTR.
s32 bnet_poll(PpuThread& t, u32 fds, s32 nfds, s32 ms)
{
    const s32 r = [&]() -> s32 {
        if (nfds <= 0) return 0;
        if (!usable(t, fds, u64{8} * static_cast<u32>(nfds), "socketpoll: fds")) return -SYS_NET_EFAULT;
        u8* base = t.kernel.base();
        struct Entry {
            s32 fd;
            u16 events, revents;
            Socket* sock;
        };
        std::vector<Entry> entries(static_cast<usize>(nfds));
        NetState& st = net(t.kernel);
        MultiWait wait;
        std::unique_lock lk(st.m);
        for (u32 i = 0; i < entries.size(); i++) {
            Entry& e = entries[i];
            e.fd = static_cast<s32>(ps_read32(base, fds + 8 * i));
            e.events = ps_read16(base, fds + 8 * i + 4);
            e.revents = 0;
            e.sock = nullptr;
            if (e.fd < 0) continue;
            if (auto sock = lookup(st, e.fd)) {
                e.sock = sock.get();
                wait.add(std::move(sock));
            }
        }
        const auto deadline = ms > 0 ? deadline_after(static_cast<u64>(ms) * 1000) : std::nullopt;
        bool timed_out = false;
        for (;;) {
            s32 count = 0;
            for (Entry& e : entries) {
                e.revents = 0;
                if (e.fd < 0) continue;
                if (!e.sock) {
                    e.revents = SYS_NET_POLLNVAL;
                } else {
                    if ((e.events & SYS_NET_POLLIN) && readable(*e.sock)) e.revents |= SYS_NET_POLLIN;
                    if ((e.events & SYS_NET_POLLOUT) && writable(*e.sock)) e.revents |= SYS_NET_POLLOUT;
                }
                if (e.revents) count++;
            }
            if (count || ms == 0 || timed_out) {
                for (u32 i = 0; i < entries.size(); i++) ps_write16(base, fds + 8 * i + 6, entries[i].revents);
                return count;
            }
            timed_out = !wait.sleep(st, t, lk, deadline);
            if (wait.interrupted()) return -SYS_NET_EINTR;
        }
    }();
    PS_LOG_DEBUG(kNetChan, "socketpoll(nfds {}, ms {}): {}", nfds, ms, result_text(r));
    return r;
}

// ------------------------------------------------------------------------------------------------- socket info

// sys_net_sockinfo_t (36 bytes): s, proto, recv_queue_length, send_queue_length, local_adr, local_port, remote_adr,
// remote_port, state. sys_net_sockinfo_ex_t (80 bytes, RPCS3 sys_net_.h) adds socket_type, local_vport, remote_vport
// and 8 reserved words. RPCS3 net_write_sockinfo: the protocol by type, the addresses from getsockname and
// getpeername, the state from get_sockinfo.
constexpr u32 kSockinfoSize = 36, kSockinfoExSize = 80;

// The state lv2 reports for one socket asked for by its descriptor: RPCS3's (lv2_socket_*::get_sockinfo), the
// SYS_NET_STATE numbers libnet hands on as they are.
s32 own_state(const Socket& sock, const SockAddr& local, const Endpoint& remote)
{
    switch (sock.type) {
    case SYS_NET_SOCK_STREAM:
        return remote.port   ? SYS_NET_STATE_ESTABLISHED
             : sock.listening ? SYS_NET_STATE_LISTEN
             : local.port     ? SYS_NET_STATE_OPENED
                              : SYS_NET_STATE_CREATED;
    case SYS_NET_SOCK_DGRAM: return local.port ? SYS_NET_STATE_OPENED : SYS_NET_STATE_CREATED;
    case SYS_NET_SOCK_DGRAM_P2P: return sock.local.vport ? SYS_NET_STATE_OPENED : SYS_NET_STATE_CREATED;
    case SYS_NET_SOCK_STREAM_P2P:
        return sock.listening    ? SYS_NET_STATE_LISTEN
             : sock.local.vport ? SYS_NET_STATE_OPENED
                                : SYS_NET_STATE_CREATED;
    }
    return SYS_NET_STATE_UNKNOWN; // raw
}

// The state lv2 reports in a list of every socket: the kernel's TCP state, which libnet maps to the SYS_NET_STATE
// numbers (a socket that is not TCP past libnet's table, which makes it OPENED).
u32 list_state(const Socket& sock)
{
    switch (sock.type) {
    case SYS_NET_SOCK_STREAM:
        return sock.connected ? TCPS_ESTABLISHED : sock.listening ? TCPS_LISTEN : TCPS_CLOSED;
    case SYS_NET_SOCK_STREAM_P2P: return sock.listening ? TCPS_LISTEN : TCPS_CLOSED;
    }
    return TCPS_NONE;
}

void write_sockinfo(u8* base, u32 at, const Socket& sock, bool ex, bool listed)
{
    const SockAddr local = local_name(sock);
    const bool native = sock.type == SYS_NET_SOCK_STREAM || sock.type == SYS_NET_SOCK_DGRAM;
    const Endpoint remote = native && sock.connected ? sock.peer : Endpoint{};
    s32 proto = sock.protocol;
    if (sock.type == SYS_NET_SOCK_DGRAM || sock.type == SYS_NET_SOCK_DGRAM_P2P) proto = SYS_NET_IPPROTO_UDP;
    if (sock.type == SYS_NET_SOCK_STREAM || sock.type == SYS_NET_SOCK_STREAM_P2P) proto = SYS_NET_IPPROTO_TCP;
    usize queued = sock.datagram_bytes;
    if (sock.type == SYS_NET_SOCK_STREAM && sock.connected) queued = sock.in->bytes.size();
    const u32 state = listed ? list_state(sock) : static_cast<u32>(own_state(sock, local, remote));
    const u32 words[] = {static_cast<u32>(sock.id), static_cast<u32>(proto), static_cast<u32>(queued), 0, local.addr,
        local.port, remote.addr, remote.port, state};
    for (u32 i = 0; i < 9; i++) ps_write32(base, at + 4 * i, words[i]);
    if (!ex) return;
    ps_write32(base, at + 36, static_cast<u32>(sock.type));
    ps_write32(base, at + 40, is_p2p(sock) ? sock.local.vport : 0);
    for (u32 off = 44; off < kSockinfoExSize; off += 4) ps_write32(base, at + off, 0); // remote_vport, reserved
}

// RPCS3 sys_net_infoctl command 6 (11 for the _ex form, which RPCS3 lacks): s == -1 reports every socket in
// descriptor order, up to n (a null p: just the count); a socket's own report needs p and n >= 1 and answers 0.
s32 bnet_sockinfo(PpuThread& t, s32 s, u32 p, s32 n, bool ex)
{
    const std::string_view call = ex ? "sys_net_get_sockinfo_ex" : "sys_net_get_sockinfo";
    const s32 r = [&]() -> s32 {
        const u32 size = ex ? kSockinfoExSize : kSockinfoSize;
        u8* base = t.kernel.base();
        NetState& st = net(t.kernel);
        std::lock_guard lock(st.m);
        if (s == -1) {
            if (!p) {
                return static_cast<s32>(std::count_if(st.table.begin(), st.table.end(), [](const auto& o) { return !!o; }));
            }
            if (n < 0) return -SYS_NET_EINVAL;
            s32 count = 0;
            for (const auto& o : st.table) {
                if (!o) continue;
                if (count >= n) break;
                const u32 at = p + static_cast<u32>(count) * size;
                if (!usable(t, at, size, std::format("{}: p", call))) return -SYS_NET_EFAULT;
                write_sockinfo(base, at, *o, ex, true);
                count++;
            }
            return count;
        }
        if (!p || n < 1) return -SYS_NET_EINVAL;
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        if (!usable(t, p, size, std::format("{}: p", call))) return -SYS_NET_EFAULT;
        write_sockinfo(base, p, *sock, ex, false);
        return 0;
    }();
    PS_LOG_DEBUG(kNetChan, "{}(s {}, p {:#x}, n {}): {}", call, s, p, n, result_text(r));
    return r;
}

// ------------------------------------------------------------------------------------------------- abort

// RPCS3 sys_net_abort: a socket's blocked calls end with EINTR (none blocked: ENOENT under the strict check, else 0);
// "all" closes every socket and answers how many; a resolver's or thread's abort has nothing to stop here (the
// resolver never blocks offline).
s32 bnet_abort(PpuThread& t, s32 type, u64 arg, s32 flags)
{
    const s32 r = [&]() -> s32 {
        NetState& st = net(t.kernel);
        std::lock_guard lock(st.m);
        switch (type) {
        case kAbortSocket: {
            const auto sock = lookup(st, static_cast<s32>(arg));
            if (!sock) return -SYS_NET_EBADF;
            if (!sock->waiting) return flags & SYS_NET_ABORT_STRICT_CHECK ? -SYS_NET_ENOENT : 0;
            sock->aborts++;
            notify_all(st);
            return 0;
        }
        case kAbortAll: {
            s32 count = 0;
            for (const auto& o : st.table) {
                if (!o) continue;
                close_locked(st, o);
                count++;
            }
            return count;
        }
        case kAbortResolver:
        case kAbortThread:
        case kAbortThreadFree: return 0;
        }
        return -SYS_NET_EINVAL;
    }();
    PS_LOG_DEBUG(kNetChan, "sys_net_abort(type {}, arg {:#x}, flags {:#x}): {}", type, arg, flags, result_text(r));
    return r;
}


// ------------------------------------------------------------------------------------------------- infoctl and the rest

// The kernel's resolver configuration (/etc/resolv.conf for libnet): the text the system set with infoctl 52, none on
// a console that never connected.

constexpr u32 kResolvConfSize = 128; // what libnet reads (infoctl 9)

// infoctl 9: the resolver configuration into buf (a NUL-terminated text, cut to len).
s32 bnet_resolv_conf(PpuThread& t, u32 buf, u32 len)
{
    if (!buf) return -SYS_NET_EINVAL; // RPCS3
    std::string text;
    {
        NetState& k = net(t.kernel);
        std::lock_guard lock(k.m);
        text = k.resolv_conf;
    }
    const u32 n = std::min<u32>(static_cast<u32>(text.size()) + 1, len);
    if (!n) return 0;
    if (!usable(t, buf, n, "infoctl 9: buf")) return -SYS_NET_EFAULT;
    u8* base = t.kernel.base();
    std::memcpy(base + buf, text.c_str(), n);
    ps_write8(base, buf + n - 1, 0);
    return 0;
}

// infoctl 52: the system sets the resolver configuration (a text).
s32 bnet_set_resolv_conf(PpuThread& t, u32 text)
{
    const auto conf = text_arg(t, text, kResolvConfSize - 1, "infoctl 52: text");
    if (!conf) return -SYS_NET_EFAULT;
    NetState& k = net(t.kernel);
    std::lock_guard lock(k.m);
    k.resolv_conf = conf->text;
    PS_LOG_INFO(kNetChan, "the resolver configuration is now \"{}\"", conf->text);
    return 0;
}

// sys_net_infoctl with libnet's argument block (libnet's variadic wrapper lays the arguments out per command, 32
// bytes). Commands RPCS3 leaves as TODOs answer CELL_OK, logged, as RPCS3's do.
s32 bnet_infoctl(PpuThread& t, s32 cmd, u32 arg)
{
    u8* base = t.kernel.base();
    const auto word = [&](u32 off) { return arg ? ps_read32(base, arg + off) : 0u; };
    if (arg && !usable(t, arg, 32, "infoctl: arg")) return -SYS_NET_EFAULT;
    switch (cmd) {
    case 6: return bnet_sockinfo(t, static_cast<s32>(word(0)), word(12), static_cast<s32>(word(16)), false);
    case 11: return bnet_sockinfo(t, static_cast<s32>(word(0)), word(12), static_cast<s32>(word(16)), true);
    case 9:
        if (!arg) return -SYS_NET_EINVAL;
        return bnet_resolv_conf(t, word(4), word(8));
    case 52: return bnet_set_resolv_conf(t, word(4));
    case 53: return 0; // a thread's libnet sync object, for the kernel to wake: nothing waits on it offline
    }
    PS_LOG_INFO(kNetChan, "sys_net_infoctl(cmd {}, words {:#x} {:#x} {:#x} {:#x}): CELL_OK (as RPCS3)", cmd, word(0),
        word(4), word(8), word(12));
    return 0;
}

// The test parameters the kernel keeps (sys_net_set_test_param and the netemu and UDP-P2P forms, through ioctl on a
// socket): a 512-byte block after a 16-byte interface name, and a 16-byte one.
constexpr u32 kIoctlGetTestParam = 0xC210698E, kIoctlSetTestParam = 0x8210698F;
constexpr u32 kIoctlGetUdpP2p = 0xC01050CE, kIoctlSetUdpP2p = 0x801050CD;

// sys_net_bnet_ioctl: the test parameters; anything else is RPCS3's stub (CELL_OK, logged).
s32 bnet_ioctl(PpuThread& t, s32 s, u32 cmd, u32 arg)
{
    const s32 r = [&]() -> s32 {
        u8* base = t.kernel.base();
        NetState& st = net(t.kernel);
        std::lock_guard lock(st.m);
        const bool test = cmd == kIoctlGetTestParam || cmd == kIoctlSetTestParam;
        const bool udp = cmd == kIoctlGetUdpP2p || cmd == kIoctlSetUdpP2p;
        if (!test && !udp) {
            PS_LOG_INFO(kNetChan, "sys_net_bnet_ioctl(s {}, cmd {:#x}, arg {:#x}): CELL_OK (RPCS3's stub)", s, cmd, arg);
            return 0;
        }
        if (!lookup(st, s)) return -SYS_NET_EBADF;
        const u32 at = test ? arg + 16 : arg;
        const u32 size = test ? 512 : 16;
        if (!usable(t, at, size, "ioctl: arg")) return -SYS_NET_EFAULT;
        u8* keep = test ? st.test_param.data() : st.udpp2p_param.data();
        if (cmd == kIoctlGetTestParam || cmd == kIoctlGetUdpP2p) std::memcpy(base + at, keep, size);
        else std::memcpy(keep, base + at, size);
        return 0;
    }();
    PS_LOG_DEBUG(kNetChan, "ioctl(s {}, {:#x}): {}", s, cmd, result_text(r));
    return r;
}

// The syscalls RPCS3 answers with a stub (sys_net_control, the dumps, sysctl, eurus_post_command): CELL_OK, logged
// (pspocket's lv2 answers them the same way, D-184).
s32 bnet_stub(std::string_view name, PpuThread& t)
{
    PS_LOG_INFO(kNetChan, "{}(r3 {:#x}, r4 {:#x}, r5 {:#x}, r6 {:#x}): CELL_OK (RPCS3's stub)", name, t.ctx->gpr[3],
        t.ctx->gpr[4], t.ctx->gpr[5], t.ctx->gpr[6]);
    return 0;
}


} // namespace

Result sys_net_bnet_accept(PpuThread& t, s32 s, u32 addr, u32 paddrlen) { return bnet_accept(t, s, addr, paddrlen); }
Result sys_net_bnet_bind(PpuThread& t, s32 s, u32 addr, u32 addrlen) { return bnet_bind(t, s, addr, addrlen); }
Result sys_net_bnet_connect(PpuThread& t, s32 s, u32 addr, u32 addrlen) { return bnet_connect(t, s, addr, addrlen); }
Result sys_net_bnet_getpeername(PpuThread& t, s32 s, u32 addr, u32 paddrlen)
{
    return bnet_sockname(t, s, addr, paddrlen, true);
}
Result sys_net_bnet_getsockname(PpuThread& t, s32 s, u32 addr, u32 paddrlen)
{
    return bnet_sockname(t, s, addr, paddrlen, false);
}
Result sys_net_bnet_getsockopt(PpuThread& t, s32 s, s32 level, s32 optname, u32 optval, u32 optlen)
{
    return bnet_getsockopt(t, s, level, optname, optval, optlen);
}
Result sys_net_bnet_listen(PpuThread& t, s32 s, s32 backlog) { return bnet_listen(t, s, backlog); }
Result sys_net_bnet_recvfrom(PpuThread& t, s32 s, u32 buf, u32 len, s32 flags, u32 addr, u32 paddrlen)
{
    return bnet_recvfrom(t, s, buf, len, flags, addr, paddrlen);
}
Result sys_net_bnet_recvmsg(PpuThread& t, s32 s, u32 msg, s32 flags) { return bnet_recvmsg(t, s, msg, flags); }
Result sys_net_bnet_sendmsg(PpuThread& t, s32 s, u32 msg, s32 flags) { return bnet_sendmsg(t, s, msg, flags); }
Result sys_net_bnet_sendto(PpuThread& t, s32 s, u32 buf, u32 len, s32 flags, u32 addr, u32 addrlen)
{
    return bnet_sendto(t, s, buf, len, flags, addr, addrlen);
}
Result sys_net_bnet_setsockopt(PpuThread& t, s32 s, s32 level, s32 optname, u32 optval, u32 optlen)
{
    return bnet_setsockopt(t, s, level, optname, optval, optlen);
}
Result sys_net_bnet_shutdown(PpuThread& t, s32 s, s32 how) { return bnet_shutdown(t, s, how); }
Result sys_net_bnet_socket(PpuThread& t, s32 family, s32 type, s32 protocol)
{
    return bnet_socket(t, family, type, protocol);
}
Result sys_net_bnet_close(PpuThread& t, s32 s) { return bnet_close(t, s); }
Result sys_net_bnet_poll(PpuThread& t, u32 fds, s32 nfds, s32 ms) { return bnet_poll(t, fds, nfds, ms); }
Result sys_net_bnet_select(PpuThread& t, s32 nfds, u32 readfds, u32 writefds, u32 exceptfds, u32 timeout)
{
    return bnet_select(t, nfds, readfds, writefds, exceptfds, timeout);
}
Result _sys_net_open_dump(PpuThread& t, s32, s32) { return bnet_stub("_sys_net_open_dump", t); }
Result _sys_net_read_dump(PpuThread& t, s32, u32, s32, u32) { return bnet_stub("_sys_net_read_dump", t); }
Result _sys_net_close_dump(PpuThread& t, s32, u32) { return bnet_stub("_sys_net_close_dump", t); }
Result _sys_net_write_dump(PpuThread& t, s32, u32, s32, u32) { return bnet_stub("_sys_net_write_dump", t); }
Result sys_net_abort(PpuThread& t, s32 type, u64 arg, s32 flags) { return bnet_abort(t, type, arg, flags); }
Result sys_net_infoctl(PpuThread& t, s32 cmd, u32 arg) { return bnet_infoctl(t, cmd, arg); }
Result sys_net_control(PpuThread& t, u32, s32, u32, s32) { return bnet_stub("sys_net_control", t); }
Result sys_net_bnet_ioctl(PpuThread& t, s32 s, u32 cmd, u32 arg) { return bnet_ioctl(t, s, cmd, arg); }
// sys_net_bnet_sysctl(name, namelen, oldp, u64* oldlenp, newp, newlen): the routing socket's tables ({CTL_NET,
// PF_ROUTE, ...}: libnet's getifaddrs and routing tables) are empty with no link: a length of 0. Anything else is
// RPCS3's stub (CELL_OK, nothing written).
Result sys_net_bnet_sysctl(PpuThread& t, u32 name, u32 namelen, u32 oldp, u32 oldlenp, u32 newp, u32 newlen)
{
    const Guest& g = t.kernel.impl().guest;
    constexpr u32 CTL_NET = 4, PF_ROUTE = 17;
    if (namelen >= 2 && g.valid(name, 8) && g.r32(name) == CTL_NET && g.r32(name + 4) == PF_ROUTE) {
        if (oldlenp && !g.valid(oldlenp, 8)) return -static_cast<s32>(SYS_NET_EFAULT);
        if (oldlenp) g.w64(oldlenp, 0);
        PS_LOG_DEBUG(kNetChan, "sysctl(CTL_NET, PF_ROUTE, ...): no interface, no route (no link)");
        return 0;
    }
    PS_LOG_INFO(kNetChan, "sys_net_bnet_sysctl(name {:#x} ({} words), old {:#x}/{:#x}, new {:#x}/{:#x}): CELL_OK (RPCS3's "
                       "stub)",
        name, namelen, oldp, oldlenp, newp, newlen);
    return 0;
}
Result sys_net_eurus_post_command(PpuThread& t, s32, u32, u32) { return bnet_stub("sys_net_eurus_post_command", t); }

void register_net_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 700, sys_net_bnet_accept);
    PS_LV2_BIND(table, 701, sys_net_bnet_bind);
    PS_LV2_BIND(table, 702, sys_net_bnet_connect);
    PS_LV2_BIND(table, 703, sys_net_bnet_getpeername);
    PS_LV2_BIND(table, 704, sys_net_bnet_getsockname);
    PS_LV2_BIND(table, 705, sys_net_bnet_getsockopt);
    PS_LV2_BIND(table, 706, sys_net_bnet_listen);
    PS_LV2_BIND(table, 707, sys_net_bnet_recvfrom);
    PS_LV2_BIND(table, 708, sys_net_bnet_recvmsg);
    PS_LV2_BIND(table, 709, sys_net_bnet_sendmsg);
    PS_LV2_BIND(table, 710, sys_net_bnet_sendto);
    PS_LV2_BIND(table, 711, sys_net_bnet_setsockopt);
    PS_LV2_BIND(table, 712, sys_net_bnet_shutdown);
    PS_LV2_BIND(table, 713, sys_net_bnet_socket);
    PS_LV2_BIND(table, 714, sys_net_bnet_close);
    PS_LV2_BIND(table, 715, sys_net_bnet_poll);
    PS_LV2_BIND(table, 716, sys_net_bnet_select);
    PS_LV2_BIND(table, 717, _sys_net_open_dump);
    PS_LV2_BIND(table, 718, _sys_net_read_dump);
    PS_LV2_BIND(table, 719, _sys_net_close_dump);
    PS_LV2_BIND(table, 720, _sys_net_write_dump);
    PS_LV2_BIND(table, 721, sys_net_abort);
    PS_LV2_BIND(table, 722, sys_net_infoctl);
    PS_LV2_BIND(table, 723, sys_net_control);
    PS_LV2_BIND(table, 724, sys_net_bnet_ioctl);
    PS_LV2_BIND(table, 725, sys_net_bnet_sysctl);
    PS_LV2_BIND(table, 726, sys_net_eurus_post_command);
}

} // namespace pspocket::lv2
