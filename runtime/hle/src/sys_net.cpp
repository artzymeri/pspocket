// sys_net (libnet): BSD sockets, address text conversions and the resolver, for a console whose network cable is
// unplugged. Online play is a non-goal (D-024): the library has to come up and behave as on a console without a link,
// consistently with cellNetCtl (state Disconnected, no IP address).
//
// References: RPCS3 rpcs3/Emu/Cell/Modules/sys_net_.cpp/.h (libnet's exports), rpcs3/Emu/Cell/lv2/sys_net.cpp/.h and
// lv2/sys_net/lv2_socket*.cpp, sys_net_helpers.cpp (the bnet syscalls libnet wraps: argument checks, error codes,
// socket IDs, select, socket info). RPCS3 runs Sony's libnet by default (g_prx_list: libnet.sprx is not HLE), so its
// sys_net_.cpp is mostly TODO stubs; the libnet layer here keeps the BSD convention libnet follows: a call that fails
// stores the SYS_NET_E* code in the calling thread's sys_net_errno (_sys_net_errno_loc) and returns -1.
//
// The offline model, a deviation from RPCS3 (which gives the game host sockets):
// - No packet leaves the process. Sockets live in a table here, numbered as RPCS3's lv2_socket IDs: 24 to 1023, in
//   increasing order until the range is used up, then the lowest free one (IdManager allocate_id).
// - socket, bind, listen, setsockopt, getsockname, shutdown, socketclose, socketselect and sys_net_get_sockinfo(_ex)
//   keep a console's socket state, with RPCS3's argument checks and error codes. With the cable unplugged the only
//   interface is the loopback one: bind takes INADDR_ANY, 127.0.0.1 and (datagram sockets) multicast groups; any
//   other address gives EADDRNOTAVAIL, as no interface has it.
// - connect, sendto and send to an address outside 127/8 fail with SYS_NET_EADDRNOTAVAIL: RPCS3's answer while its
//   network is disconnected (lv2_socket_native::connect/sendto, for public addresses; here nothing is reachable).
// - Loopback works in memory, between the game's own sockets. A UDP datagram (a P2P one by virtual port) reaches the
//   socket bound to its destination, within that socket's SO_RCVBUF (else it is dropped, as BSD drops it). A TCP
//   connect to 127/8 reaches a listening socket of the game (ECONNREFUSED when there is none or its backlog is full)
//   and completes at once; the connection carries bytes both ways, up to SO_RCVBUF + SO_SNDBUF in flight per
//   direction, with BSD's EOF and EPIPE (a close with unread data gives the peer EOF, not a reset). P2P stream
//   sockets and raw sockets have no loopback peer.
// - recv, recvfrom and accept on a non-blocking socket (SO_NBIO, MSG_DONTWAIT) answer EWOULDBLOCK. On a blocking
//   socket they wait as a console's do: for loopback data, the socket's shutdown, a socketclose (EINTR, RPCS3's
//   abort_socket), its SO_RCVTIMEO (EWOULDBLOCK, as RPCS3; accept honours it as well), or the process exit. A console
//   without a link never receives anything else, so a blocking call without a timeout on a socket the game doesn't
//   feed itself sleeps for good, as it does there: a receive thread parked in recvfrom is what the game expects
//   offline, and an error a console never returns would rather send such a thread round a busy loop. The wait shows
//   in pspocket-boot's thread report as a sleep in the HLE function, and the first one on each socket is logged.
// - socketselect waits up to its timeout the same way; exceptfds is never set (as RPCS3), and the count is of sockets
//   with an event, not of bits (as RPCS3).
// - gethostbyname: there is no name server, so a name fails with h_errno TRY_AGAIN, the BSD resolver's answer when no
//   server can be asked (cellHttp's CELL_HTTP_ERROR_NET_RESOLVER_TRY_AGAIN). A dotted-quad name resolves without a
//   name server, as in the BSD resolver.
// - inet_addr, inet_pton and inet_ntop are BSD's (FreeBSD libc/inet, from BIND): pure functions, exactly.
//
// Other deviations: pointers are checked before use (D-042): one that is not guest memory is logged with the call site
// and the call fails with SYS_NET_EFAULT, the BSD answer (RPCS3 faults). What RPCS3 asserts on or throws for (stream
// and datagram sockets of another family or protocol, unknown flags, connect, listen or accept on P2P datagram and raw
// sockets) is logged as an error and refused. sys_net_get_sockinfo reports the bytes queued for reading (RPCS3 0).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/object.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sys_net";
using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------------------------------------------- constants
// RPCS3 lv2/sys_net.h (its SYS_NET_ names; the numbers are BSD's).

enum : s32 {
    SYS_NET_EINTR = 4,
    SYS_NET_EBADF = 9,
    SYS_NET_EACCES = 13,
    SYS_NET_EFAULT = 14,
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

// sys_net_sockinfo_t::state.
enum : s32 {
    SYS_NET_STATE_UNKNOWN = 0,
    SYS_NET_STATE_CREATED = 2,
    SYS_NET_STATE_OPENED = 3,
    SYS_NET_STATE_LISTEN = 4,
    SYS_NET_STATE_ESTABLISHED = 7,
};

// h_errno (BSD netdb.h; cellHttp's CELL_HTTP_ERROR_NET_RESOLVER_* carry the same numbers).
enum : s32 {
    SYS_NET_NETDB_INTERNAL = -1,
    SYS_NET_HOST_NOT_FOUND = 1,
    SYS_NET_TRY_AGAIN = 2,
};

constexpr u32 kIdBase = 24, kIdCount = 1000; // RPCS3 lv2_socket id_base, id_count
constexpr u32 kLoopback = 0x7F000001;        // 127.0.0.1
constexpr u32 kInaddrNone = 0xFFFFFFFF;
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
    case SYS_NET_EINTR: return "SYS_NET_EINTR";
    case SYS_NET_EBADF: return "SYS_NET_EBADF";
    case SYS_NET_EACCES: return "SYS_NET_EACCES";
    case SYS_NET_EFAULT: return "SYS_NET_EFAULT";
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
    case SYS_NET_EISCONN: return "SYS_NET_EISCONN";
    case SYS_NET_ENOTCONN: return "SYS_NET_ENOTCONN";
    case SYS_NET_ECONNREFUSED: return "SYS_NET_ECONNREFUSED";
    }
    return "?";
}

// What a libnet call returns, for logs: the value, or -1 and the errno it stores.
std::string result_text(s32 r)
{
    if (r >= 0) return std::to_string(r);
    return std::format("-1, errno {}", errno_name(-r));
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

// Whether all of [addr, addr + size) is committed guest memory (checked per host page).
bool range_ok(lv2::PpuThread& t, u32 addr, u64 size)
{
    if (!size) return true;
    if (u64{addr} + size > GuestMemory::kGuestSize) return false;
    const u64 page = GuestMemory::host_page_size();
    for (u64 a = addr & ~(page - 1); a < u64{addr} + size; a += page)
        if (!t.kernel.memory().is_committed(static_cast<u32>(a))) return false;
    return true;
}

// A pointer argument checked before use (D-042): if it is not guest memory the call site is logged, and the call
// fails with EFAULT (RPCS3 faults).
bool usable(PPUContext& ctx, u32 addr, u64 size, std::string_view what)
{
    if (range_ok(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

struct GuestText {
    std::string text;
    bool terminated = false; // a NUL within the bytes read
};

// The NUL-terminated string at `addr`, at most `max` characters (terminated false when longer); nullopt (logged) when
// it runs into memory that isn't committed first.
std::optional<GuestText> text_arg(PPUContext& ctx, u32 addr, u32 max, std::string_view what)
{
    lv2::PpuThread& t = thread_of(ctx);
    const u64 page = GuestMemory::host_page_size();
    GuestText out;
    for (u32 i = 0;; i++) {
        const u64 a = u64{addr} + i;
        if (a >= GuestMemory::kGuestSize ||
            ((i == 0 || a % page == 0) && !t.kernel.memory().is_committed(static_cast<u32>(a)))) {
            log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}) is not a string in guest memory", what, addr));
            return std::nullopt;
        }
        const char c = static_cast<char>(ps_read8(t.kernel.base(), static_cast<u32>(a)));
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

struct Options {
    s32 nbio = 0;
    s32 broadcast = 0;
    s32 reuseaddr = 0;
    s32 reuseport = 0;
    u32 sndbuf = kDefaultSndBuf;
    u32 rcvbuf = kDefaultRcvBufDgram;
    u64 rcvtimeo_us = 0; // 0: none
    u64 sndtimeo_us = 0;
};

struct Socket {
    s32 id = 0; // descriptor; 0 while a connection waits in its listener's queue
    s32 family = 0;
    s32 type = 0;
    s32 protocol = 0;
    bool closed = false; // socketclose ran: its waiters end with EINTR
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
};

bool is_p2p(const Socket& s) { return s.type == SYS_NET_SOCK_DGRAM_P2P || s.type == SYS_NET_SOCK_STREAM_P2P; }
bool reusable(const Socket& s) { return s.opt.reuseaddr || s.opt.reuseport; }

// The module's guest page: gethostbyname's answer, then errno areas for threads without TLS.
constexpr u32 kPageSize = 0x4000;
constexpr u32 kHostentArea = 0, kFallbackAreas = 0x400;
constexpr u32 kFallbackCount = (kPageSize - kFallbackAreas) / kTlsSystemArea;

struct NetState {
    std::mutex m; // guards everything below
    std::array<std::shared_ptr<Socket>, kIdCount> table{};
    u32 highest = 0; // slots handed out in order so far (RPCS3 IdManager highest_index)
    u32 next_ephemeral = 0;
    u16 next_p2ps_vport = 0;
    lv2::WaitQueue waiters; // threads blocked in this module, woken (all) by every state change
    u32 page = 0;
    std::unordered_map<u32, u32> fallback; // lv2 thread ID -> its stand-in system area
    u32 fallback_used = 0;
    bool except_logged = false;
};

NetState& net() { return HleProcess::current()->state<NetState>(); }

// With st.m held.
u32 module_page(NetState& st)
{
    if (!st.page) st.page = HleProcess::current()->main_memory().alloc(kPageSize);
    if (!st.page) PS_FATAL(kChan, "no guest memory for sys_net's page");
    return st.page;
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
    for (lv2::Waiter* w : st.waiters.take_all(lv2::SYS_SYNC_FIFO)) lv2::wake(*w, lv2::CELL_OK);
}

std::optional<Clock::time_point> deadline_after(u64 us)
{
    if (!us) return std::nullopt;
    return lv2::deadline_after_us(us);
}

// Parks the calling thread until the next notify_all, or until `deadline` (none: no timeout). `lk` holds st.m on
// entry and on return. False when the deadline passed. The process exiting unwinds the thread (lv2::ThreadExit), as
// it does a blocked syscall.
bool sleep_until_notified(NetState& st, PPUContext& ctx, std::unique_lock<std::mutex>& lk,
    const std::optional<Clock::time_point>& deadline)
{
    u64 timeout_us = 0;
    if (deadline) {
        const auto now = Clock::now();
        if (now >= *deadline) return false;
        timeout_us = static_cast<u64>(
            std::max<s64>(1, std::chrono::ceil<std::chrono::microseconds>(*deadline - now).count()));
    }
    lv2::Waiter& w = thread_of(ctx).waiter;
    lv2::prepare_wait(w);
    st.waiters.push(&w);
    const lv2::WaitStatus status = lv2::park(w, lk, timeout_us, {lv2::WaitKind::Sleep, 0}, [&](bool) {
        return st.waiters.remove(&w) ? lv2::CancelResult::Removed : lv2::CancelResult::Woken;
    });
    if (status == lv2::WaitStatus::Aborted) throw lv2::ThreadExit{};
    lk.lock();
    return status == lv2::WaitStatus::Woken;
}

void log_wait_once(Socket& sock, std::string_view call, const std::optional<Clock::time_point>& deadline)
{
    if (deadline || sock.wait_logged) return;
    sock.wait_logged = true;
    PS_LOG_INFO(kChan, "{}(s {}): a blocking socket without a timeout; with no network it waits for loopback data, a "
                       "shutdown, a socketclose or the process exit",
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

// ------------------------------------------------------------------------------------------------- errno

// RPCS3 _sys_net_errno_loc / _sys_net_h_errno_loc: the calling thread's TLS system area (r13 - 0x7030, cleared for
// every thread by ppu_alloc_tls), errno at + 0x2c and h_errno at + 0x28. A thread whose r13 doesn't point past a
// committed system area (no TLS: an HLE helper or test thread) gets a stand-in area of its own in sys_net's page.
constexpr u32 kErrnoOffset = 0x2c, kHErrnoOffset = 0x28;

u32 system_area(PPUContext& ctx)
{
    lv2::PpuThread& t = thread_of(ctx);
    const u32 r13 = static_cast<u32>(ctx.gpr[13]);
    if (r13 >= kTlsR13Offset && guest_readable(t, r13 - kTlsR13Offset, kTlsSystemArea)) return r13 - kTlsR13Offset;
    NetState& st = net();
    std::lock_guard lock(st.m);
    if (const auto it = st.fallback.find(t.id); it != st.fallback.end()) return it->second;
    const u32 area = module_page(st) + kFallbackAreas + (st.fallback_used++ % kFallbackCount) * kTlsSystemArea;
    std::memset(t.kernel.memory().host(area), 0, kTlsSystemArea);
    st.fallback[t.id] = area;
    PS_LOG_WARN(kChan, "thread {:#x} has no TLS (r13 {:#x}): its sys_net_errno is at {:#010x}", t.id, ctx.gpr[13],
        area + kErrnoOffset);
    return area;
}

void set_errno(PPUContext& ctx, s32 e)
{
    ps_write32(guest_base(ctx), system_area(ctx) + kErrnoOffset, static_cast<u32>(e));
}

void set_h_errno(PPUContext& ctx, s32 e)
{
    ps_write32(guest_base(ctx), system_area(ctx) + kHErrnoOffset, static_cast<u32>(e));
}

// libnet's convention for a call's result (an lv2 bnet result: a value, or -SYS_NET_E*).
s32 finish(PPUContext& ctx, s32 r)
{
    if (r >= 0) return r;
    set_errno(ctx, -r);
    return -1;
}

// ------------------------------------------------------------------------------------------------- socket, close

// RPCS3 sys_net_bnet_socket: the type first; RPCS3 only logs another family. For stream and datagram sockets it
// asserts on another family or protocol (lv2_socket_native::create_socket), and a host's socket() refuses a TCP
// datagram or UDP stream socket with EPROTONOSUPPORT.
s32 socket(PPUContext& ctx, s32 family, s32 type, s32 protocol)
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
                PS_LOG_ERROR(kChan, "socket: family {} for a {} socket (RPCS3 asserts)", family, type_name(type));
                return -SYS_NET_EAFNOSUPPORT;
            }
            if (protocol != SYS_NET_IPPROTO_IP && protocol != own) {
                PS_LOG_ERROR(kChan, "socket: protocol {} for a {} socket", protocol, type_name(type));
                return -SYS_NET_EPROTONOSUPPORT;
            }
        } else if (family != SYS_NET_AF_INET) {
            PS_LOG_ERROR(kChan, "socket: family {} for a {} socket; created anyway, as RPCS3", family, type_name(type));
        }
        auto sock = std::make_shared<Socket>();
        sock->family = family;
        sock->type = type;
        sock->protocol = protocol;
        if (type == SYS_NET_SOCK_STREAM || type == SYS_NET_SOCK_STREAM_P2P) sock->opt.rcvbuf = kDefaultRcvBufStream;
        NetState& st = net();
        std::lock_guard lock(st.m);
        const s32 id = insert(st, sock);
        return id ? id : -SYS_NET_EMFILE; // RPCS3: "Can't allocate more than 1000 sockets"
    }();
    PS_LOG_INFO(kChan, "socket(family {}, {}, protocol {}): {}", family, type_name(type), protocol, result_text(r));
    return finish(ctx, r);
}

// RPCS3 sys_net_bnet_close. A thread blocked on the socket wakes with EINTR (abort_socket).
s32 socketclose(PPUContext& ctx, s32 s)
{
    const s32 r = [&]() -> s32 {
        NetState& st = net();
        std::lock_guard lock(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        st.table[static_cast<usize>(s) - kIdBase] = nullptr;
        close_socket(*sock);
        notify_all(st);
        return 0;
    }();
    PS_LOG_INFO(kChan, "socketclose(s {}): {}", s, result_text(r));
    return finish(ctx, r);
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
s32 bind(PPUContext& ctx, s32 s, u32 addr, u32 addrlen)
{
    SockAddr sa{};
    const s32 r = [&]() -> s32 {
        if (!addr || addrlen < kSockaddrSize) return -SYS_NET_EINVAL;
        if (!usable(ctx, addr, kSockaddrSize, "bind: addr")) return -SYS_NET_EFAULT;
        sa = read_sockaddr(guest_base(ctx), addr);
        NetState& st = net();
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
    PS_LOG_INFO(kChan, "bind(s {}, {}): {}", s, sockaddr_text(sa), result_text(r));
    return finish(ctx, r);
}

// RPCS3 sys_net_bnet_listen: a positive backlog, then the socket. A datagram socket can't listen (BSD; RPCS3 aborts
// for a P2P one), an unbound stream socket gets an ephemeral port (BSD tcp_usr_listen), a connected one can't
// (BSD solisten). RPCS3's P2P stream socket listens without checks; its raw one ignores it.
s32 listen(PPUContext& ctx, s32 s, s32 backlog)
{
    const s32 r = [&]() -> s32 {
        if (backlog <= 0) return -SYS_NET_EINVAL;
        NetState& st = net();
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
            PS_LOG_ERROR(kChan, "listen on a DGRAM-P2P socket (RPCS3: fatal)");
            return -SYS_NET_EOPNOTSUPP;
        default: return 0;
        }
        sock->listening = true;
        sock->backlog = std::min(static_cast<u32>(backlog), kSomaxconn);
        return 0;
    }();
    PS_LOG_INFO(kChan, "listen(s {}, backlog {}): {}", s, backlog, result_text(r));
    return finish(ctx, r);
}

// RPCS3 sys_net_bnet_getsockname: both pointers, a length of at least 16, then the socket; the length comes back 16.
s32 getsockname(PPUContext& ctx, s32 s, u32 addr, u32 paddrlen)
{
    SockAddr sa{};
    const s32 r = [&]() -> s32 {
        if (!addr || !paddrlen) return -SYS_NET_EINVAL;
        if (!usable(ctx, paddrlen, 4, "getsockname: paddrlen")) return -SYS_NET_EFAULT;
        u8* base = guest_base(ctx);
        if (ps_read32(base, paddrlen) < kSockaddrSize) return -SYS_NET_EINVAL;
        if (!usable(ctx, addr, kSockaddrSize, "getsockname: addr")) return -SYS_NET_EFAULT;
        NetState& st = net();
        std::lock_guard lock(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        sa = local_name(*sock);
        write_sockaddr(base, addr, sa);
        ps_write32(base, paddrlen, kSockaddrSize);
        return 0;
    }();
    PS_LOG_DEBUG(kChan, "getsockname(s {}): {}, {}", s, result_text(r), sockaddr_text(sa));
    return finish(ctx, r);
}

// ------------------------------------------------------------------------------------------------- setsockopt

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

// The options of lv2_socket_native::setsockopt (RPCS3), on the state kept here; an option RPCS3 hands to the host is
// checked as BSD checks it, and those without an effect here are dropped. RPCS3's P2P sockets keep any option without
// checks (SO_NBIO and SO_BROADCAST take effect) and its raw ones take SO_NBIO; here both take SO_NBIO, SO_BROADCAST,
// the reuse options and the timeouts that way.
s32 set_option(u8* base, Socket& sock, s32 level, s32 optname, u32 optval, u32 optlen)
{
    const s32 value = static_cast<s32>(ps_read32(base, optval));
    if (is_p2p(sock) || sock.type == SYS_NET_SOCK_RAW) {
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
        case SYS_NET_SO_KEEPALIVE: return 0;
        case SYS_NET_SO_SNDBUF:
        case SYS_NET_SO_RCVBUF:
            if (value < 1) return -SYS_NET_EINVAL;
            (optname == SYS_NET_SO_SNDBUF ? sock.opt.sndbuf : sock.opt.rcvbuf) = static_cast<u32>(value);
            return 0;
        case SYS_NET_SO_SNDLOWAT:
        case SYS_NET_SO_RCVLOWAT: return value < 1 ? -SYS_NET_EINVAL : 0;
        case SYS_NET_SO_BROADCAST: sock.opt.broadcast = value; return 0;
        case SYS_NET_SO_REUSEADDR: sock.opt.reuseaddr = value; return 0;
        case SYS_NET_SO_REUSEPORT: sock.opt.reuseport = value; return 0;
        case SYS_NET_SO_SNDTIMEO:
        case SYS_NET_SO_RCVTIMEO:
            if (optlen < 16) return -SYS_NET_EINVAL;
            return set_timeout(base, optval, optname == SYS_NET_SO_SNDTIMEO ? sock.opt.sndtimeo_us : sock.opt.rcvtimeo_us);
        case SYS_NET_SO_LINGER: return optlen < 8 ? -SYS_NET_EINVAL : 0;
        case SYS_NET_SO_USECRYPTO:
        case SYS_NET_SO_USESIGNATURE:
            PS_LOG_WARN(kChan, "setsockopt(s {}): option {:#x} accepted without effect (RPCS3's stub)", sock.id, optname);
            return 0;
        }
        PS_LOG_ERROR(kChan, "setsockopt(s {}, SOL_SOCKET): unknown option {:#x}", sock.id, optname);
        return -SYS_NET_EINVAL;
    case SYS_NET_IPPROTO_TCP:
        if (optname == SYS_NET_TCP_MAXSEG) return 0; // RPCS3: kept, no effect
        if (optname == SYS_NET_TCP_NODELAY) return sock.type == SYS_NET_SOCK_STREAM ? 0 : -SYS_NET_EINVAL;
        PS_LOG_ERROR(kChan, "setsockopt(s {}, IPPROTO_TCP): unknown option {:#x}", sock.id, optname);
        return -SYS_NET_EINVAL;
    case SYS_NET_IPPROTO_IP:
        switch (optname) {
        case SYS_NET_IP_HDRINCL:
        case SYS_NET_IP_TOS:
        case SYS_NET_IP_TTL:
        case SYS_NET_IP_MULTICAST_IF:
        case SYS_NET_IP_MULTICAST_TTL:
        case SYS_NET_IP_MULTICAST_LOOP:
        case SYS_NET_IP_TTLCHK:
        case SYS_NET_IP_MAXTTL:
        case SYS_NET_IP_DONTFRAG: return 0;
        case SYS_NET_IP_ADD_MEMBERSHIP:
        case SYS_NET_IP_DROP_MEMBERSHIP:
            if (optlen < 8) return -SYS_NET_EINVAL;
            return membership(sock, optname == SYS_NET_IP_ADD_MEMBERSHIP, ps_read32(base, optval),
                ps_read32(base, optval + 4));
        }
        PS_LOG_ERROR(kChan, "setsockopt(s {}, IPPROTO_IP): unknown option {:#x}", sock.id, optname);
        return -SYS_NET_EINVAL;
    }
    PS_LOG_ERROR(kChan, "setsockopt(s {}): unknown level {:#x}", sock.id, level);
    return -SYS_NET_EINVAL;
}

// RPCS3 sys_net_bnet_setsockopt: a length of at least 4 first, then the socket.
s32 setsockopt(PPUContext& ctx, s32 s, s32 level, s32 optname, u32 optval, u32 optlen)
{
    u32 value = 0;
    const s32 r = [&]() -> s32 {
        if (optlen < 4) return -SYS_NET_EINVAL;
        if (!usable(ctx, optval, optlen, "setsockopt: optval")) return -SYS_NET_EFAULT;
        u8* base = guest_base(ctx);
        value = ps_read32(base, optval);
        NetState& st = net();
        std::lock_guard lock(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        return set_option(base, *sock, level, optname, optval, optlen);
    }();
    PS_LOG_INFO(kChan, "setsockopt(s {}, level {:#x}, option {:#x}, value {:#x}, optlen {}): {}", s, level, optname,
        value, optlen, result_text(r));
    return finish(ctx, r);
}

// ------------------------------------------------------------------------------------------------- shutdown

// RPCS3 sys_net_bnet_shutdown: `how` first, then the socket. A socket that isn't connected answers ENOTCONN (BSD
// soshutdown; the host behind RPCS3 does the same). RPCS3's P2P sockets treat it as a TODO and its raw ones as a stub.
s32 shutdown(PPUContext& ctx, s32 s, s32 how)
{
    const s32 r = [&]() -> s32 {
        if (how < SYS_NET_SHUT_RD || how > SYS_NET_SHUT_RDWR) return -SYS_NET_EINVAL;
        NetState& st = net();
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
    PS_LOG_INFO(kChan, "shutdown(s {}, how {}): {}", s, how, result_text(r));
    return finish(ctx, r);
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
s32 connect(PPUContext& ctx, s32 s, u32 addr, u32 addrlen)
{
    SockAddr sa{};
    const s32 r = [&]() -> s32 {
        if (!addr || addrlen < kSockaddrSize) return -SYS_NET_EINVAL;
        if (!usable(ctx, addr, kSockaddrSize, "connect: addr")) return -SYS_NET_EFAULT;
        sa = read_sockaddr(guest_base(ctx), addr);
        if (sa.family != SYS_NET_AF_INET) return -SYS_NET_EAFNOSUPPORT;
        NetState& st = net();
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
        PS_LOG_ERROR(kChan, "connect on a {} socket (RPCS3: fatal)", type_name(sock->type));
        return -SYS_NET_EOPNOTSUPP;
    }();
    PS_LOG_INFO(kChan, "connect(s {}, {}): {}", s, sockaddr_text(sa), result_text(r));
    return finish(ctx, r);
}

// RPCS3 sys_net_bnet_accept: addr and paddrlen both or neither, a length of at least 16, then the socket. The new
// socket inherits the listener's options; its descriptor is allocated when it is accepted (EMFILE leaves it queued).
s32 accept(PPUContext& ctx, s32 s, u32 addr, u32 paddrlen)
{
    SockAddr peer{};
    const s32 r = [&]() -> s32 {
        if ((addr != 0) != (paddrlen != 0)) return -SYS_NET_EINVAL;
        u8* base = guest_base(ctx);
        if (paddrlen) {
            if (!usable(ctx, paddrlen, 4, "accept: paddrlen")) return -SYS_NET_EFAULT;
            if (ps_read32(base, paddrlen) < kSockaddrSize) return -SYS_NET_EINVAL;
            if (!usable(ctx, addr, kSockaddrSize, "accept: addr")) return -SYS_NET_EFAULT;
        }
        NetState& st = net();
        std::unique_lock lk(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        switch (sock->type) {
        case SYS_NET_SOCK_STREAM:
        case SYS_NET_SOCK_STREAM_P2P: break;
        case SYS_NET_SOCK_DGRAM: return -SYS_NET_EOPNOTSUPP;
        default:
            PS_LOG_ERROR(kChan, "accept on a {} socket (RPCS3: fatal)", type_name(sock->type));
            return -SYS_NET_EOPNOTSUPP;
        }
        if (!sock->listening) return -SYS_NET_EINVAL; // BSD: accept on a socket that doesn't listen
        const auto deadline = deadline_after(sock->opt.rcvtimeo_us);
        for (;;) {
            if (sock->closed) return -SYS_NET_EINTR;
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
            if (!sleep_until_notified(st, ctx, lk, deadline)) return -SYS_NET_EWOULDBLOCK;
        }
    }();
    if (r == -SYS_NET_EWOULDBLOCK) {
        PS_LOG_DEBUG(kChan, "accept(s {}): {}", s, result_text(r));
    } else {
        PS_LOG_INFO(kChan, "accept(s {}): {}{}", s, result_text(r), r >= 0 ? " from " + sockaddr_text(peer) : "");
    }
    return finish(ctx, r);
}

// ------------------------------------------------------------------------------------------------- receiving

// recv on a TCP socket: what is queued, up to `len`; with MSG_WAITALL until `len` bytes (or EOF, a timeout, an
// error). EOF reads 0. Not connected (listening, or never connected): ENOTCONN (BSD soreceive).
s32 recv_stream(PPUContext& ctx, NetState& st, std::unique_lock<std::mutex>& lk, Socket& sock, u32 buf, u32 len,
    s32 flags, bool nonblock, const std::optional<Clock::time_point>& deadline)
{
    if (!sock.connected) return -SYS_NET_ENOTCONN;
    const bool peek = flags & SYS_NET_MSG_PEEK;
    const bool waitall = (flags & SYS_NET_MSG_WAITALL) && !peek;
    u8* base = guest_base(ctx);
    u32 got = 0;
    const auto partial = [&](s32 error) { return got ? static_cast<s32>(got) : error; };
    for (;;) {
        if (sock.closed) return partial(-SYS_NET_EINTR);
        if (sock.shut_rd) return static_cast<s32>(got);
        Pipe& in = *sock.in;
        const u32 n = static_cast<u32>(std::min<usize>(in.bytes.size(), len - got));
        if (n) {
            if (!usable(ctx, buf + got, n, "recv: buf")) return partial(-SYS_NET_EFAULT);
            std::copy_n(in.bytes.begin(), n, base + buf + got);
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
        if (!sleep_until_notified(st, ctx, lk, deadline)) return partial(-SYS_NET_EWOULDBLOCK);
    }
}

// recvfrom on a UDP or P2P datagram socket: the next datagram, cut to `len` (the rest is dropped unless MSG_PEEK).
s32 recv_dgram(PPUContext& ctx, NetState& st, std::unique_lock<std::mutex>& lk, Socket& sock, u32 buf, u32 len,
    s32 flags, bool nonblock, const std::optional<Clock::time_point>& deadline, Endpoint& from)
{
    for (;;) {
        if (sock.closed) return -SYS_NET_EINTR;
        if (!sock.datagrams.empty()) {
            const Datagram& d = sock.datagrams.front();
            const u32 n = static_cast<u32>(std::min<usize>(len, d.data.size()));
            if (!usable(ctx, buf, n, "recvfrom: buf")) return -SYS_NET_EFAULT;
            std::memcpy(guest_base(ctx) + buf, d.data.data(), n);
            from = d.from;
            if (!(flags & SYS_NET_MSG_PEEK)) {
                sock.datagram_bytes -= d.data.size();
                sock.datagrams.pop_front();
            }
            return static_cast<s32>(n);
        }
        if (sock.shut_rd) return 0;
        if (nonblock) return -SYS_NET_EWOULDBLOCK;
        log_wait_once(sock, "recvfrom", deadline);
        if (!sleep_until_notified(st, ctx, lk, deadline)) return -SYS_NET_EWOULDBLOCK;
    }
}

// A socket nothing can reach (raw, a listening P2P stream socket): EWOULDBLOCK, or a wait for the timeout or close.
s32 recv_nothing(PPUContext& ctx, NetState& st, std::unique_lock<std::mutex>& lk, Socket& sock, bool nonblock,
    const std::optional<Clock::time_point>& deadline)
{
    for (;;) {
        if (sock.closed) return -SYS_NET_EINTR;
        if (nonblock) return -SYS_NET_EWOULDBLOCK;
        log_wait_once(sock, "recvfrom", deadline);
        if (!sleep_until_notified(st, ctx, lk, deadline)) return -SYS_NET_EWOULDBLOCK;
    }
}

// RPCS3 sys_net_bnet_recvfrom: a buffer, a length, addr and paddrlen both or neither, known flags, then the socket. A
// timeout (SO_RCVTIMEO) ends with EWOULDBLOCK. The address comes back as RPCS3 writes it: the source of a datagram, a
// zero port and address for stream data, all zero for a result of 0; the length 16.
s32 net_recv(PPUContext& ctx, std::string_view call, s32 s, u32 buf, u32 len, s32 flags, u32 addr, u32 paddrlen)
{
    const s32 r = [&]() -> s32 {
        if (!buf || !len || (addr != 0) != (paddrlen != 0)) return -SYS_NET_EINVAL;
        if (flags & ~kRecvFlags) {
            PS_LOG_ERROR(kChan, "{}(s {}): unknown flags {:#x} (RPCS3 aborts)", call, s, flags);
            return -SYS_NET_EINVAL;
        }
        if (addr && (!usable(ctx, paddrlen, 4, "recvfrom: paddrlen") ||
                        !usable(ctx, addr, kSockaddrSize, "recvfrom: addr"))) {
            return -SYS_NET_EFAULT;
        }
        NetState& st = net();
        std::unique_lock lk(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        const bool nonblock = sock->opt.nbio || (flags & SYS_NET_MSG_DONTWAIT);
        const auto deadline = deadline_after(sock->opt.rcvtimeo_us);
        Endpoint from{};
        s32 n = 0;
        switch (sock->type) {
        case SYS_NET_SOCK_STREAM: n = recv_stream(ctx, st, lk, *sock, buf, len, flags, nonblock, deadline); break;
        case SYS_NET_SOCK_DGRAM:
        case SYS_NET_SOCK_DGRAM_P2P:
            n = recv_dgram(ctx, st, lk, *sock, buf, len, flags, nonblock, deadline, from);
            break;
        case SYS_NET_SOCK_STREAM_P2P:
            if (!sock->listening) return -SYS_NET_ENOTCONN; // RPCS3 lv2_socket_p2ps::recvfrom on a closed stream
            [[fallthrough]];
        default: n = recv_nothing(ctx, st, lk, *sock, nonblock, deadline); break;
        }
        if (n >= 0 && addr) {
            SockAddr sa{};
            if (n > 0) {
                const bool dgram = sock->type == SYS_NET_SOCK_DGRAM || sock->type == SYS_NET_SOCK_DGRAM_P2P;
                sa = {kSockaddrSize, SYS_NET_AF_INET, dgram ? from.port : u16{0}, dgram ? from.addr : 0,
                    sock->type == SYS_NET_SOCK_DGRAM_P2P ? from.vport : u16{0}};
            }
            u8* base = guest_base(ctx);
            write_sockaddr(base, addr, sa);
            ps_write32(base, paddrlen, kSockaddrSize);
        }
        return n;
    }();
    PS_LOG_DEBUG(kChan, "{}(s {}, len {:#x}, flags {:#x}): {}", call, s, len, flags, result_text(r));
    return r;
}

s32 recvfrom(PPUContext& ctx, s32 s, u32 buf, u32 len, s32 flags, u32 addr, u32 paddrlen)
{
    return finish(ctx, net_recv(ctx, "recvfrom", s, buf, len, flags, addr, paddrlen));
}

s32 recv(PPUContext& ctx, s32 s, u32 buf, u32 len, s32 flags)
{
    return finish(ctx, net_recv(ctx, "recv", s, buf, len, flags, 0, 0));
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
        PS_LOG_DEBUG(kChan, "loopback datagram of {} bytes to {}:{} dropped: {}", len, ip_text(dest.addr), dest.port,
            dropped);
        return;
    }
    to->datagrams.push_back({src, std::vector<u8>(data, data + len)});
    to->datagram_bytes += len;
}

// send on a TCP socket: RPCS3's offline check of a given address first; then into the connection, as much as its
// buffers take, waiting for room on a blocking socket (SO_SNDTIMEO ends the wait with what was sent, or EWOULDBLOCK).
s32 send_stream(PPUContext& ctx, NetState& st, std::unique_lock<std::mutex>& lk, Socket& sock, u32 buf, u32 len,
    bool nonblock, const std::optional<SockAddr>& to)
{
    if (to && !loopback(to->addr)) return -SYS_NET_EADDRNOTAVAIL;
    if (!sock.connected) return -SYS_NET_ENOTCONN;
    const auto deadline = deadline_after(sock.opt.sndtimeo_us);
    const u8* src = guest_base(ctx) + buf;
    u32 sent = 0;
    const auto partial = [&](s32 error) { return sent ? static_cast<s32>(sent) : error; };
    for (;;) {
        if (sock.closed) return partial(-SYS_NET_EINTR);
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
        if (!sleep_until_notified(st, ctx, lk, deadline)) return partial(-SYS_NET_EWOULDBLOCK);
    }
}

// sendto on a UDP socket: RPCS3's offline check, then BSD udp_output (an address and a connection exclude each other;
// a datagram fits SO_SNDBUF and IP; port 0 is no destination); an unbound sender is bound implicitly.
s32 send_dgram(PPUContext& ctx, NetState& st, Socket& sock, u32 buf, u32 len, const std::optional<SockAddr>& to)
{
    if (to && !loopback(to->addr)) return -SYS_NET_EADDRNOTAVAIL;
    if (to && sock.connected) return -SYS_NET_EISCONN;
    if (!to && !sock.connected) return -SYS_NET_EDESTADDRREQ;
    if (sock.shut_wr) return -SYS_NET_EPIPE;
    if (len > std::min(sock.opt.sndbuf, kUdpMaxPayload)) return -SYS_NET_EMSGSIZE;
    const Endpoint dest = to ? Endpoint{normalized(to->addr), to->port, 0} : sock.peer;
    if (!dest.port) return -SYS_NET_EADDRNOTAVAIL;
    if (!bind_implicitly(st, sock)) return -SYS_NET_EADDRNOTAVAIL;
    deliver(st, sock, dest, guest_base(ctx) + buf, len);
    notify_all(st);
    return static_cast<s32>(len);
}

// sendto on a P2P datagram socket (RPCS3 lv2_socket_p2p::sendto, which asserts an address, a bound socket and a
// payload that fits; refuses a broadcast without SO_BROADCAST), then the offline check.
s32 send_p2p(PPUContext& ctx, NetState& st, Socket& sock, u32 buf, u32 len, const std::optional<SockAddr>& to)
{
    if (!to || !sock.bound || len > kP2pMaxPayload) {
        PS_LOG_ERROR(kChan, "sendto(s {}): {} (RPCS3 asserts)", sock.id,
            !to ? "no address" : !sock.bound ? "socket not bound" : "payload too large");
        return !to ? -SYS_NET_EDESTADDRREQ : !sock.bound ? -SYS_NET_EINVAL : -SYS_NET_EMSGSIZE;
    }
    if (to->addr == kInaddrBroadcast && !sock.opt.broadcast) return -SYS_NET_EACCES;
    if (!loopback(to->addr)) return -SYS_NET_EADDRNOTAVAIL;
    deliver(st, sock, {normalized(to->addr), to->port, to->vport}, guest_base(ctx) + buf, len);
    notify_all(st);
    return static_cast<s32>(len);
}

// RPCS3 sys_net_bnet_sendto: known flags, an address of at least 16 bytes and AF_INET when given, the buffer (copied
// whole), then the socket.
s32 net_send(PPUContext& ctx, std::string_view call, s32 s, u32 buf, u32 len, s32 flags, u32 addr, u32 addrlen)
{
    std::optional<SockAddr> to;
    const s32 r = [&]() -> s32 {
        if (flags & ~kSendFlags) {
            PS_LOG_ERROR(kChan, "{}(s {}): unknown flags {:#x} (RPCS3 aborts)", call, s, flags);
            return -SYS_NET_EINVAL;
        }
        if (addr) {
            if (addrlen < kSockaddrSize) return -SYS_NET_EINVAL;
            if (!usable(ctx, addr, kSockaddrSize, "sendto: addr")) return -SYS_NET_EFAULT;
            to = read_sockaddr(guest_base(ctx), addr);
            if (to->family != SYS_NET_AF_INET) return -SYS_NET_EAFNOSUPPORT;
        }
        if (!usable(ctx, buf, len, "sendto: buf")) return -SYS_NET_EFAULT;
        NetState& st = net();
        std::unique_lock lk(st.m);
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        const bool nonblock = sock->opt.nbio || (flags & SYS_NET_MSG_DONTWAIT);
        switch (sock->type) {
        case SYS_NET_SOCK_STREAM: return send_stream(ctx, st, lk, *sock, buf, len, nonblock, to);
        case SYS_NET_SOCK_DGRAM: return send_dgram(ctx, st, *sock, buf, len, to);
        case SYS_NET_SOCK_DGRAM_P2P: return send_p2p(ctx, st, *sock, buf, len, to);
        case SYS_NET_SOCK_STREAM_P2P: return -SYS_NET_ENOTCONN; // RPCS3 lv2_socket_p2ps::sendto on a closed stream
        }
        // Raw: RPCS3's stub takes everything; nothing receives raw packets here.
        if (to && !loopback(to->addr)) return -SYS_NET_EADDRNOTAVAIL;
        return static_cast<s32>(len);
    }();
    PS_LOG_DEBUG(kChan, "{}(s {}, len {:#x}, flags {:#x}{}): {}", call, s, len, flags,
        to ? ", to " + sockaddr_text(*to) : "", result_text(r));
    return r;
}

s32 sendto(PPUContext& ctx, s32 s, u32 buf, u32 len, s32 flags, u32 addr, u32 addrlen)
{
    return finish(ctx, net_send(ctx, "sendto", s, buf, len, flags, addr, addrlen));
}

s32 send(PPUContext& ctx, s32 s, u32 buf, u32 len, s32 flags)
{
    return finish(ctx, net_send(ctx, "send", s, buf, len, flags, 0, 0));
}

// ------------------------------------------------------------------------------------------------- select

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

// RPCS3 sys_net_bnet_select: the timeout (tv_sec * 1000000 + tv_usec, unsigned; none: forever; 0: a poll), 0 < nfds
// <= 1024, a socket for every descriptor below nfds set in readfds or writefds (EBADF otherwise). The result counts the
// sockets with an event and replaces the sets; exceptfds comes back empty. A selected socket closed during the wait
// ends it with EINTR.
s32 socketselect(PPUContext& ctx, s32 nfds, u32 readfds, u32 writefds, u32 exceptfds, u32 timeout)
{
    std::optional<u64> timeout_us;
    const s32 r = [&]() -> s32 {
        u8* base = guest_base(ctx);
        if (timeout) {
            if (!usable(ctx, timeout, 16, "socketselect: timeout")) return -SYS_NET_EFAULT;
            timeout_us = ps_read64(base, timeout) * 1000000u + ps_read64(base, timeout + 8);
        }
        NetState& st = net();
        if (exceptfds) {
            std::lock_guard lock(st.m);
            if (!st.except_logged) PS_LOG_WARN(kChan, "socketselect: exceptfds is never set (as RPCS3)");
            st.except_logged = true;
        }
        if (nfds <= 0 || nfds > 1024) return -SYS_NET_EINVAL;
        FdSet rset{}, wset{};
        if (readfds) {
            if (!usable(ctx, readfds, kFdSetSize, "socketselect: readfds")) return -SYS_NET_EFAULT;
            rset = read_fdset(base, readfds);
        }
        if (writefds) {
            if (!usable(ctx, writefds, kFdSetSize, "socketselect: writefds")) return -SYS_NET_EFAULT;
            wset = read_fdset(base, writefds);
        }
        if (exceptfds && !usable(ctx, exceptfds, kFdSetSize, "socketselect: exceptfds")) return -SYS_NET_EFAULT;

        struct Selected {
            s32 fd;
            std::shared_ptr<Socket> sock;
            bool read, write;
        };
        std::vector<Selected> selected;
        std::unique_lock lk(st.m);
        for (s32 fd = 0; fd < nfds; fd++) {
            const bool rd = readfds && has_fd(rset, fd), wr = writefds && has_fd(wset, fd);
            if (!rd && !wr) continue;
            auto sock = lookup(st, fd);
            if (!sock) return -SYS_NET_EBADF;
            selected.push_back({fd, std::move(sock), rd, wr});
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
            timed_out = !sleep_until_notified(st, ctx, lk, deadline);
            for (const Selected& x : selected)
                if (x.sock->closed) return -SYS_NET_EINTR;
        }
    }();
    PS_LOG_DEBUG(kChan, "socketselect(nfds {}, timeout {}): {}", nfds,
        timeout_us ? std::format("{} us", *timeout_us) : std::string("none"), result_text(r));
    return finish(ctx, r);
}

// ------------------------------------------------------------------------------------------------- socket info

// sys_net_sockinfo_t (36 bytes): s, proto, recv_queue_length, send_queue_length, local_adr, local_port, remote_adr,
// remote_port, state. sys_net_sockinfo_ex_t (80 bytes, RPCS3 sys_net_.h) adds socket_type, local_vport, remote_vport
// and 8 reserved words. RPCS3 net_write_sockinfo: the protocol by type, the addresses from getsockname and
// getpeername, the state from get_sockinfo.
constexpr u32 kSockinfoSize = 36, kSockinfoExSize = 80;

void write_sockinfo(u8* base, u32 at, const Socket& sock, bool ex)
{
    const SockAddr local = local_name(sock);
    const bool native = sock.type == SYS_NET_SOCK_STREAM || sock.type == SYS_NET_SOCK_DGRAM;
    const Endpoint remote = native && sock.connected ? sock.peer : Endpoint{};
    s32 proto = sock.protocol;
    if (sock.type == SYS_NET_SOCK_DGRAM || sock.type == SYS_NET_SOCK_DGRAM_P2P) proto = SYS_NET_IPPROTO_UDP;
    if (sock.type == SYS_NET_SOCK_STREAM || sock.type == SYS_NET_SOCK_STREAM_P2P) proto = SYS_NET_IPPROTO_TCP;
    usize queued = sock.datagram_bytes;
    if (sock.type == SYS_NET_SOCK_STREAM && sock.connected) queued = sock.in->bytes.size();
    s32 state = SYS_NET_STATE_UNKNOWN; // raw
    switch (sock.type) {
    case SYS_NET_SOCK_STREAM:
        state = remote.port   ? SYS_NET_STATE_ESTABLISHED
              : sock.listening ? SYS_NET_STATE_LISTEN
              : local.port     ? SYS_NET_STATE_OPENED
                               : SYS_NET_STATE_CREATED;
        break;
    case SYS_NET_SOCK_DGRAM: state = local.port ? SYS_NET_STATE_OPENED : SYS_NET_STATE_CREATED; break;
    case SYS_NET_SOCK_DGRAM_P2P: state = sock.local.vport ? SYS_NET_STATE_OPENED : SYS_NET_STATE_CREATED; break;
    case SYS_NET_SOCK_STREAM_P2P:
        state = sock.listening    ? SYS_NET_STATE_LISTEN
              : sock.local.vport ? SYS_NET_STATE_OPENED
                                 : SYS_NET_STATE_CREATED;
        break;
    }
    const u32 words[] = {static_cast<u32>(sock.id), static_cast<u32>(proto), static_cast<u32>(queued), 0, local.addr,
        local.port, remote.addr, remote.port, static_cast<u32>(state)};
    for (u32 i = 0; i < 9; i++) ps_write32(base, at + 4 * i, words[i]);
    if (!ex) return;
    ps_write32(base, at + 36, static_cast<u32>(sock.type));
    ps_write32(base, at + 40, is_p2p(sock) ? sock.local.vport : 0);
    for (u32 off = 44; off < kSockinfoExSize; off += 4) ps_write32(base, at + off, 0); // remote_vport, reserved
}

// RPCS3 sys_net_infoctl command 6 (what libnet's sys_net_get_sockinfo asks lv2): s == -1 reports every socket in
// descriptor order, up to n (a null p: just the count); a socket's own report needs p and n >= 1 and answers 0.
s32 net_sockinfo(PPUContext& ctx, std::string_view call, s32 s, u32 p, s32 n, bool ex)
{
    const s32 r = [&]() -> s32 {
        const u32 size = ex ? kSockinfoExSize : kSockinfoSize;
        u8* base = guest_base(ctx);
        NetState& st = net();
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
                if (!usable(ctx, at, size, "sys_net_get_sockinfo: p")) return -SYS_NET_EFAULT;
                write_sockinfo(base, at, *o, ex);
                count++;
            }
            return count;
        }
        if (!p || n < 1) return -SYS_NET_EINVAL;
        const auto sock = lookup(st, s);
        if (!sock) return -SYS_NET_EBADF;
        if (!usable(ctx, p, size, "sys_net_get_sockinfo: p")) return -SYS_NET_EFAULT;
        write_sockinfo(base, p, *sock, ex);
        return 0;
    }();
    PS_LOG_DEBUG(kChan, "{}(s {}, p {:#x}, n {}): {}", call, s, p, n, result_text(r));
    return finish(ctx, r);
}

s32 sys_net_get_sockinfo(PPUContext& ctx, s32 s, u32 p, s32 n)
{
    return net_sockinfo(ctx, "sys_net_get_sockinfo", s, p, n, false);
}

// RPCS3 has no arguments for it (UNIMPLEMENTED_FUNC) but defines its structure; taken as sys_net_get_sockinfo with a
// fourth argument (flags), which is only logged.
s32 sys_net_get_sockinfo_ex(PPUContext& ctx, s32 s, u32 p, s32 n, s32 flags)
{
    PS_LOG_DEBUG(kChan, "sys_net_get_sockinfo_ex: flags {:#x}", flags);
    return net_sockinfo(ctx, "sys_net_get_sockinfo_ex", s, p, n, true);
}

// ------------------------------------------------------------------------------------------------- address text
// BSD's conversions (FreeBSD libc/inet: inet_addr.c's inet_aton is BIND 4.9's, inet_pton.c and inet_ntop.c BIND 9's),
// in the C locale.

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool is_xdigit(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
constexpr bool is_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
constexpr u32 xdigit_value(char c)
{
    return static_cast<u32>(is_digit(c) ? c - '0' : (c >= 'a' ? c - 'a' + 10 : c - 'A' + 10));
}

// inet_aton: one to four parts in decimal, octal (a leading 0) or hex (0x), dot-separated; the last fills the bytes
// left; trailing white space ends it. A part accumulates in a 64-bit u_long (the PPU's), and the address is its low
// 32 bits. Host order.
std::optional<u32> aton(std::string_view s)
{
    usize i = 0;
    const auto at = [&](usize k) { return k < s.size() ? s[k] : '\0'; };
    std::array<u64, 3> parts{};
    usize count = 0;
    u64 val = 0;
    bool digit = false;
    char c = at(i);
    for (;;) {
        if (!is_digit(c)) return std::nullopt;
        val = 0;
        u32 base = 10;
        digit = false;
        if (c == '0') {
            c = at(++i);
            if (c == 'x' || c == 'X') {
                base = 16;
                c = at(++i);
            } else {
                base = 8;
                digit = true;
            }
        }
        for (;;) {
            if (is_digit(c)) {
                if (base == 8 && (c == '8' || c == '9')) return std::nullopt;
                val = val * base + static_cast<u64>(c - '0');
                c = at(++i);
                digit = true;
            } else if (base == 16 && is_xdigit(c)) {
                val = (val << 4) | xdigit_value(c);
                c = at(++i);
                digit = true;
            } else {
                break;
            }
        }
        if (c != '.') break;
        if (count >= parts.size() || val > 0xFF) return std::nullopt;
        parts[count++] = val;
        c = at(++i);
    }
    if (c != '\0' && !is_space(c)) return std::nullopt;
    if (!digit) return std::nullopt;
    switch (count + 1) {
    case 2:
        if (val > 0xFFFFFF) return std::nullopt;
        val |= parts[0] << 24;
        break;
    case 3:
        if (val > 0xFFFF) return std::nullopt;
        val |= parts[0] << 24 | parts[1] << 16;
        break;
    case 4:
        if (val > 0xFF) return std::nullopt;
        val |= parts[0] << 24 | parts[1] << 16 | parts[2] << 8;
        break;
    default: break;
    }
    return static_cast<u32>(val);
}

// inet_pton4: exactly four decimal octets of 0..255, without leading zeros.
std::optional<std::array<u8, 4>> pton4(std::string_view s)
{
    std::array<u8, 4> out{};
    bool saw_digit = false;
    u32 octets = 0;
    usize k = 0;
    for (const char ch : s) {
        if (is_digit(ch)) {
            const u32 next = out[k] * 10u + static_cast<u32>(ch - '0');
            if (saw_digit && out[k] == 0) return std::nullopt;
            if (next > 255) return std::nullopt;
            out[k] = static_cast<u8>(next);
            if (!saw_digit) {
                if (++octets > 4) return std::nullopt;
                saw_digit = true;
            }
        } else if (ch == '.' && saw_digit) {
            if (octets == 4) return std::nullopt;
            out[++k] = 0;
            saw_digit = false;
        } else {
            return std::nullopt;
        }
    }
    if (octets < 4) return std::nullopt;
    return out;
}

// inet_pton6: up to eight groups of one to four hex digits, one "::" for a run of zero groups, an IPv4 dotted quad
// for the last 32 bits.
std::optional<std::array<u8, 16>> pton6(std::string_view s)
{
    std::array<u8, 16> tmp{};
    constexpr usize kEnd = 16;
    usize tp = 0;
    std::optional<usize> colonp;
    usize i = 0;
    if (!s.empty() && s[0] == ':') { // a leading "::"
        if (s.size() < 2 || s[1] != ':') return std::nullopt;
        i = 1;
    }
    usize curtok = i;
    u32 seen_xdigits = 0;
    u32 val = 0;
    while (i < s.size()) {
        const char ch = s[i++];
        if (is_xdigit(ch)) {
            if (++seen_xdigits > 4) return std::nullopt;
            val = (val << 4) | xdigit_value(ch);
            continue;
        }
        if (ch == ':') {
            curtok = i;
            if (!seen_xdigits) {
                if (colonp) return std::nullopt;
                colonp = tp;
                continue;
            }
            if (i == s.size()) return std::nullopt;
            if (tp + 2 > kEnd) return std::nullopt;
            tmp[tp++] = static_cast<u8>(val >> 8);
            tmp[tp++] = static_cast<u8>(val);
            seen_xdigits = 0;
            val = 0;
            continue;
        }
        if (ch == '.' && tp + 4 <= kEnd) {
            if (const auto v4 = pton4(s.substr(curtok))) {
                std::copy(v4->begin(), v4->end(), tmp.begin() + static_cast<std::ptrdiff_t>(tp));
                tp += 4;
                seen_xdigits = 0;
                break;
            }
        }
        return std::nullopt;
    }
    if (seen_xdigits) {
        if (tp + 2 > kEnd) return std::nullopt;
        tmp[tp++] = static_cast<u8>(val >> 8);
        tmp[tp++] = static_cast<u8>(val);
    }
    if (colonp) {
        if (tp == kEnd) return std::nullopt;
        const usize n = tp - *colonp;
        for (usize k = 1; k <= n; k++) {
            tmp[kEnd - k] = tmp[*colonp + n - k];
            tmp[*colonp + n - k] = 0;
        }
        tp = kEnd;
    }
    if (tp != kEnd) return std::nullopt;
    return tmp;
}

std::string ntop4(const u8* a) { return std::format("{}.{}.{}.{}", a[0], a[1], a[2], a[3]); }

// inet_ntop6: lowercase hex groups without leading zeros; the longest run of two or more zero groups (the first of
// equals) becomes "::"; an IPv4-compatible or -mapped address ends in a dotted quad.
std::string ntop6(const u8* a)
{
    std::array<u32, 8> words{};
    for (u32 i = 0; i < 16; i++) words[i / 2] |= u32{a[i]} << ((1 - (i % 2)) << 3);
    struct Run {
        s32 base = -1, len = 0;
    } best, cur;
    for (s32 i = 0; i < 8; i++) {
        if (words[static_cast<usize>(i)] == 0) {
            if (cur.base == -1) cur = {i, 1};
            else cur.len++;
        } else if (cur.base != -1) {
            if (best.base == -1 || cur.len > best.len) best = cur;
            cur.base = -1;
        }
    }
    if (cur.base != -1 && (best.base == -1 || cur.len > best.len)) best = cur;
    if (best.base != -1 && best.len < 2) best.base = -1;
    std::string out;
    for (s32 i = 0; i < 8; i++) {
        if (best.base != -1 && i >= best.base && i < best.base + best.len) {
            if (i == best.base) out += ':';
            continue;
        }
        if (i != 0) out += ':';
        if (i == 6 && best.base == 0 &&
            (best.len == 6 || (best.len == 7 && words[7] != 0x0001) || (best.len == 5 && words[5] == 0xffff))) {
            out += ntop4(a + 12);
            break;
        }
        out += std::format("{:x}", words[static_cast<usize>(i)]);
    }
    if (best.base != -1 && best.base + best.len == 8) out += ':';
    return out;
}

// inet_addr(const char* cp): the address in network order (the PPU's own), INADDR_NONE when cp isn't one.
u32 inet_addr(PPUContext& ctx, u32 cp)
{
    const auto text = text_arg(ctx, cp, 4096, "inet_addr: cp");
    const auto a = text && text->terminated ? aton(text->text) : std::nullopt;
    PS_LOG_DEBUG(kChan, "inet_addr(\"{}\"): {}", text ? text->text : "?", a ? ip_text(*a) : "INADDR_NONE");
    return a ? *a : kInaddrNone;
}

// inet_pton(af, src, dst): 1 and the address in dst, 0 when src isn't one, -1 (EAFNOSUPPORT) for another family.
s32 inet_pton(PPUContext& ctx, s32 af, u32 src, u32 dst)
{
    std::string text;
    const s32 r = [&]() -> s32 {
        if (af != SYS_NET_AF_INET && af != SYS_NET_AF_INET6) return -SYS_NET_EAFNOSUPPORT;
        const auto t = text_arg(ctx, src, 64, "inet_pton: src"); // longer than any address
        if (!t) return -SYS_NET_EFAULT;
        text = t->text;
        if (!t->terminated) return 0;
        std::vector<u8> bytes;
        if (af == SYS_NET_AF_INET) {
            const auto a = pton4(text);
            if (!a) return 0;
            bytes.assign(a->begin(), a->end());
        } else {
            const auto a = pton6(text);
            if (!a) return 0;
            bytes.assign(a->begin(), a->end());
        }
        if (!usable(ctx, dst, bytes.size(), "inet_pton: dst")) return -SYS_NET_EFAULT;
        std::memcpy(guest_base(ctx) + dst, bytes.data(), bytes.size());
        return 1;
    }();
    PS_LOG_DEBUG(kChan, "inet_pton({}, \"{}\"): {}", af, text, result_text(r));
    return finish(ctx, r);
}

// inet_ntop(af, src, dst, size): dst, or NULL with ENOSPC (the text and its NUL don't fit `size`) or EAFNOSUPPORT.
u32 inet_ntop(PPUContext& ctx, s32 af, u32 src, u32 dst, u32 size)
{
    std::string text;
    s32 error = 0;
    const u32 r = [&]() -> u32 {
        const auto fail = [&](s32 e) {
            error = e;
            return 0u;
        };
        u8* base = guest_base(ctx);
        const u32 bytes = af == SYS_NET_AF_INET ? 4 : af == SYS_NET_AF_INET6 ? 16 : 0;
        if (!bytes) return fail(SYS_NET_EAFNOSUPPORT);
        if (!usable(ctx, src, bytes, "inet_ntop: src")) return fail(SYS_NET_EFAULT);
        text = bytes == 4 ? ntop4(base + src) : ntop6(base + src);
        if (text.size() + 1 > size) return fail(SYS_NET_ENOSPC);
        if (!usable(ctx, dst, text.size() + 1, "inet_ntop: dst")) return fail(SYS_NET_EFAULT);
        std::memcpy(base + dst, text.c_str(), text.size() + 1);
        return dst;
    }();
    if (!r) set_errno(ctx, error);
    PS_LOG_DEBUG(kChan, "inet_ntop({}, size {}): \"{}\"{}", af, size, text,
        r ? "" : std::format(", NULL, errno {}", errno_name(error)));
    return r;
}

// ------------------------------------------------------------------------------------------------- resolver

// gethostbyname(name). BSD's resolver (gethnamaddr.c) answers a name of digits and dots that doesn't end in a dot
// without a lookup: inet_pton takes it (an answer in a static hostent) or it fails with HOST_NOT_FOUND. Any other name
// needs a name server and there is none: TRY_AGAIN. h_errno is kept beside errno (RPCS3 _sys_net_h_errno_loc).
//
// The hostent (20 bytes: be32 h_name, h_aliases, h_addrtype, h_length, h_addr_list) and what it points to live in
// sys_net's page, overwritten by the next call as BSD's static one is.
u32 gethostbyname(PPUContext& ctx, u32 name)
{
    const auto text = text_arg(ctx, name, 1025, "gethostbyname: name"); // MAXDNAME
    if (!text) {
        set_h_errno(ctx, SYS_NET_NETDB_INTERNAL);
        set_errno(ctx, SYS_NET_EFAULT);
        return 0;
    }
    const std::string& n = text->text;
    const bool numeric = text->terminated && !n.empty() && is_digit(n[0]) && n.back() != '.' &&
                         std::all_of(n.begin(), n.end(), [](char c) { return is_digit(c) || c == '.'; });
    if (!numeric) {
        PS_LOG_INFO(kChan, "gethostbyname(\"{}\"): no name server (no network); NULL, h_errno TRY_AGAIN", n);
        set_h_errno(ctx, SYS_NET_TRY_AGAIN);
        return 0;
    }
    const auto a = pton4(n);
    if (!a) {
        PS_LOG_INFO(kChan, "gethostbyname(\"{}\"): not an address; NULL, h_errno HOST_NOT_FOUND", n);
        set_h_errno(ctx, SYS_NET_HOST_NOT_FOUND);
        return 0;
    }
    u32 h = 0;
    {
        NetState& st = net();
        std::lock_guard lock(st.m);
        h = module_page(st) + kHostentArea;
    }
    u8* base = guest_base(ctx);
    const u32 aliases = h + 0x20, addr_list = h + 0x28, addr = h + 0x30, hostname = h + 0x40;
    ps_write32(base, h + 0, hostname);
    ps_write32(base, h + 4, aliases);
    ps_write32(base, h + 8, SYS_NET_AF_INET);
    ps_write32(base, h + 12, 4);
    ps_write32(base, h + 16, addr_list);
    ps_write32(base, aliases, 0);
    ps_write32(base, addr_list, addr);
    ps_write32(base, addr_list + 4, 0);
    std::memcpy(base + addr, a->data(), 4);
    std::memcpy(base + hostname, n.c_str(), n.size() + 1); // at most 15 characters
    PS_LOG_INFO(kChan, "gethostbyname(\"{}\"): a numeric address, no lookup; hostent at {:#010x}", n, h);
    return h;
}

// RPCS3: UNIMPLEMENTED_FUNC, CELL_OK; its arguments are not known (logged as registers). There is no name server to
// configure.
s32 sys_net_set_resolver_configurations(PPUContext& ctx)
{
    PS_LOG_INFO(kChan, "sys_net_set_resolver_configurations(r3 {:#x}, r4 {:#x}, r5 {:#x}, r6 {:#x}): no name server; "
                       "CELL_OK (as RPCS3)",
        ctx.gpr[3], ctx.gpr[4], ctx.gpr[5], ctx.gpr[6]);
    return lv2::CELL_OK;
}

// RPCS3 sys_net_.cpp: the location of the calling thread's sys_net_errno (see system_area).
u32 _sys_net_errno_loc(PPUContext& ctx) { return system_area(ctx) + kErrnoOffset; }

// ------------------------------------------------------------------------------------------------- setup

// RPCS3: a TODO that returns CELL_OK (the parameter only sizes libnet's memory pool).
s32 sys_net_initialize_network_ex(PPUContext&, u32 param)
{
    PS_LOG_INFO(kChan, "sys_net_initialize_network_ex({:#x}): no network; returns CELL_OK (as RPCS3)", param);
    return lv2::CELL_OK;
}

// RPCS3: a TODO that returns CELL_OK. The sockets stay as they are.
s32 sys_net_finalize_network(PPUContext&)
{
    PS_LOG_INFO(kChan, "sys_net_finalize_network(): CELL_OK (as RPCS3)");
    return lv2::CELL_OK;
}

// RPCS3: UNIMPLEMENTED, returns CELL_OK. A debug printout of the network interfaces.
s32 sys_net_show_ifconfig(PPUContext&)
{
    PS_LOG_DEBUG(kChan, "sys_net_show_ifconfig(): no interfaces");
    return lv2::CELL_OK;
}

// sys_net_free_thread_context(u64 tid, s32 flags). RPCS3: a TODO that returns CELL_OK. libnet frees the context it
// keeps for a thread that used it (its errno and resolver state); here that is the thread's TLS (or a stand-in area
// kept for the process).
s32 sys_net_free_thread_context(PPUContext&, u64 tid, s32 flags)
{
    PS_LOG_DEBUG(kChan, "sys_net_free_thread_context(tid {:#x}, flags {}): no thread context; CELL_OK (as RPCS3)", tid,
        flags);
    return lv2::CELL_OK;
}

} // namespace

void register_sys_net(Registry& r)
{
    r.add("sys_net", {
        PS_HLE_FUNC(0x139A9E9B, sys_net_initialize_network_ex),
        PS_HLE_FUNC(0xB48636C4, sys_net_show_ifconfig),
        PS_HLE_FUNC(0xFDB8F926, sys_net_free_thread_context),
        PS_HLE_FUNC(0xB68D5625, sys_net_finalize_network),
        PS_HLE_FUNC(0x9C056962, socket),
        PS_HLE_FUNC(0x6DB6E8CD, socketclose),
        PS_HLE_FUNC(0xB0A59804, bind),
        PS_HLE_FUNC(0x28E208BB, listen),
        PS_HLE_FUNC(0x13EFE7F5, getsockname),
        PS_HLE_FUNC(0x88F03575, setsockopt),
        PS_HLE_FUNC(0xA50777C6, shutdown),
        PS_HLE_FUNC(0x64F66D35, connect),
        PS_HLE_FUNC(0xC94F6939, accept),
        PS_HLE_FUNC(0x1F953B9F, recvfrom),
        PS_HLE_FUNC(0xFBA04F37, recv),
        PS_HLE_FUNC(0x9647570B, sendto),
        PS_HLE_FUNC(0xDC751B40, send),
        PS_HLE_FUNC(0x3F09E20A, socketselect),
        PS_HLE_FUNC(0x3B27C780, sys_net_get_sockinfo),
        PS_HLE_FUNC(0xA765D029, sys_net_get_sockinfo_ex),
        PS_HLE_FUNC(0xDABBC2C0, inet_addr),
        PS_HLE_FUNC(0x8AF3825E, inet_pton),
        PS_HLE_FUNC(0xC98A3146, inet_ntop),
        PS_HLE_FUNC(0x71F4C717, gethostbyname),
        PS_HLE_FUNC(0x7687D48C, sys_net_set_resolver_configurations),
        PS_HLE_FUNC(0x6005CDE1, _sys_net_errno_loc),
    });
}

} // namespace pspocket::hle
