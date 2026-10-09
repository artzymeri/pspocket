// sys_net (libnet over lv2's network calls, D-523) on a console without a network, cellHttp and cellSsl (synthetic: no
// game data): descriptors as lv2 numbers them, libnet's SYS_NET_ERROR_* answers and errno in the thread's context,
// bind/listen/getsockname/setsockopt with RPCS3's checks, connect and
// sendto failing as with the cable unplugged, would-block and timeouts, loopback UDP and TCP between the game's own
// sockets, socketselect, socket info, the address conversions, gethostbyname without a name server, and the cellHttp
// and cellSsl state machines.

#include "hle_test_util.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pspocket/lv2/object.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace std::chrono_literals;

namespace {

constexpr u32 kUnmapped = 0x0FFF0000; // in the main area, never committed by these tests
constexpr u32 kNetPool = kData + 0x200000; // libnet's memory pool

// RPCS3 lv2/sys_net.h.
constexpr s32 SYS_NET_EINTR = 4, SYS_NET_EBADF = 9, SYS_NET_EACCES = 13, SYS_NET_EFAULT = 14, SYS_NET_EINVAL = 22,
              SYS_NET_EMFILE = 24, SYS_NET_ENOSPC = 28, SYS_NET_EPIPE = 32, SYS_NET_EDOM = 33,
              SYS_NET_EWOULDBLOCK = 35, SYS_NET_EDESTADDRREQ = 39, SYS_NET_EMSGSIZE = 40,
              SYS_NET_EPROTONOSUPPORT = 43, SYS_NET_EOPNOTSUPP = 45, SYS_NET_EAFNOSUPPORT = 47,
              SYS_NET_EADDRINUSE = 48, SYS_NET_EADDRNOTAVAIL = 49, SYS_NET_EISCONN = 56, SYS_NET_ENOTCONN = 57,
              SYS_NET_ECONNREFUSED = 61;
constexpr s32 kStream = 1, kDgram = 2, kRaw = 3, kDgramP2p = 6, kStreamP2p = 10;
constexpr s32 kAfInet = 2, kAfInet6 = 24, kIpprotoIp = 0, kIpprotoTcp = 6, kIpprotoUdp = 17;
constexpr s32 kSolSocket = 0xffff, kSoReuseaddr = 0x4, kSoKeepalive = 0x8, kSoBroadcast = 0x20, kSoLinger = 0x80,
              kSoReuseport = 0x200, kSoUsecrypto = 0x1000, kSoUsesignature = 0x2000, kSoSndbuf = 0x1001,
              kSoRcvbuf = 0x1002, kSoSndlowat = 0x1003, kSoRcvlowat = 0x1004, kSoSndtimeo = 0x1005,
              kSoRcvtimeo = 0x1006, kSoNbio = 0x1100;
constexpr s32 kTcpNodelay = 1, kTcpMaxseg = 2;
constexpr s32 kIpTos = 3, kIpTtl = 4, kIpMulticastTtl = 10, kIpMulticastLoop = 11, kIpAddMembership = 12,
              kIpDropMembership = 13, kIpTtlchk = 23, kIpMaxttl = 24, kIpDontfrag = 26;
constexpr s32 kMsgPeek = 0x2, kMsgWaitall = 0x40, kMsgDontwait = 0x80;
constexpr u32 kLoopback = 0x7F000001;

// A libnet call's outcome: its result and, when that is -1, the errno it left.
struct Outcome {
    s32 value = 0;
    s32 error = 0;
    bool operator==(const Outcome&) const = default;
};

std::ostream& operator<<(std::ostream& os, const Outcome& o)
{
    return os << "{" << o.value << ", errno " << o.error << "}";
}

Outcome ok(s32 v = 0) { return {v, 0}; }
Outcome fail(s32 e) { return {-1, e}; }

// A guest thread using libnet, with a TLS block of its own (errno and h_errno where RPCS3 keeps them) and its own
// scratch memory: thread `index` uses kData + 0x20000 * index (below the stacks at kStacks).
struct Net {
    GuestThread& g;
    u8* b;
    u32 tls = 0, addr = 0, addr2 = 0, len = 0, opt = 0, text = 0, sets = 0, tv = 0, info = 0, out = 0, in = 0;

    Net(GuestThread& t, u32 index) : g(t), b(t.f.base())
    {
        if (index == 0) { // libnet over a 512 KB pool (sys_net_initialize_network_ex {memory, size, flags})
            REQUIRE(t.f.hle->link_internal_functions({"sys_net"}));
            const u32 param = kData + 0x1FF000;
            ps_write32(b, param, kNetPool);
            ps_write32(b, param + 4, 0x80000);
            ps_write32(b, param + 8, 0);
            REQUIRE(t.call("sys_net_initialize_network_ex", param) == 0);
        }
        const u32 s = kData + 0x20000 * index;
        addr = s;
        addr2 = s + 0x20;
        len = s + 0x40;
        opt = s + 0x60;
        text = s + 0x100;
        sets = s + 0x200; // read, write, except: 128 bytes each
        tv = s + 0x400;
        info = s + 0x500;
        out = s + 0x1000; // 64 KB
        in = s + 0x11000; // 60 KB
        tls = kData + 0x1F0000 + 0x100 * index;
        std::memset(b + tls, 0, 0x100);
        g.ctx.gpr[13] = tls + hle::kTlsR13Offset;
    }

    s32 err() { return static_cast<s32>(ps_read32(b, static_cast<u32>(g.call("_sys_net_errno_loc")))); }

    // libnet answers a failure with SYS_NET_ERROR_* (0x80010200 | errno) and keeps the errno: as {-1, errno}.
    template <class... A>
    Outcome call(std::string_view name, A... args)
    {
        const s32 v = static_cast<s32>(g.call(name, args...));
        if ((static_cast<u32>(v) & 0xFFFFFF00) != 0x80010200) return {v, v == -1 ? err() : 0};
        const s32 e = err();
        CHECK(e == (v & 0xFF));
        return {-1, e};
    }

    s32 socket(s32 type, s32 protocol = 0)
    {
        const Outcome o = call("socket", kAfInet, type, protocol);
        REQUIRE(o.value >= 24);
        return o.value;
    }

    void put_addr(u32 at, u32 ip, u16 port, u16 vport = 0, u8 family = kAfInet)
    {
        std::memset(b + at, 0, 16);
        ps_write8(b, at, 16);
        ps_write8(b, at + 1, family);
        ps_write16(b, at + 2, port);
        ps_write32(b, at + 4, ip);
        ps_write16(b, at + 8, vport);
    }

    Outcome bind(s32 s, u32 ip, u16 port, u16 vport = 0)
    {
        put_addr(addr, ip, port, vport);
        return call("bind", s, addr, 16);
    }

    Outcome connect(s32 s, u32 ip, u16 port, u16 vport = 0)
    {
        put_addr(addr, ip, port, vport);
        return call("connect", s, addr, 16);
    }

    Outcome setopt(s32 s, s32 level, s32 name, s32 value)
    {
        ps_write32(b, opt, static_cast<u32>(value));
        return call("setsockopt", s, level, name, opt, 4);
    }

    Outcome set_timeout(s32 s, s32 name, s64 sec, s64 usec)
    {
        ps_write64(b, opt, static_cast<u64>(sec));
        ps_write64(b, opt + 8, static_cast<u64>(usec));
        return call("setsockopt", s, kSolSocket, name, opt, 16);
    }

    // getsockname: the port and address (it must succeed).
    std::pair<u16, u32> name_of(s32 s)
    {
        ps_write32(b, len, 16);
        REQUIRE(call("getsockname", s, addr2, len) == ok());
        return {ps_read16(b, addr2 + 2), ps_read32(b, addr2 + 4)};
    }

    void put_data(std::string_view data) { std::memcpy(b + out, data.data(), data.size()); }

    Outcome sendto(s32 s, std::string_view data, u32 ip, u16 port, s32 flags = 0, u16 vport = 0)
    {
        put_data(data);
        put_addr(addr, ip, port, vport);
        return call("sendto", s, out, static_cast<u32>(data.size()), flags, addr, 16);
    }

    Outcome send(s32 s, std::string_view data, s32 flags = 0)
    {
        put_data(data);
        return call("send", s, out, static_cast<u32>(data.size()), flags);
    }

    // recvfrom into `in`, the source into addr2.
    Outcome recvfrom(s32 s, u32 size, s32 flags = 0)
    {
        std::memset(b + addr2, 0xAA, 16);
        ps_write32(b, len, 0xAAAAAAAA);
        return call("recvfrom", s, in, size, flags, addr2, len);
    }

    Outcome recv(s32 s, u32 size, s32 flags = 0) { return call("recv", s, in, size, flags); }

    std::string received(s32 n) const { return std::string(reinterpret_cast<const char*>(b + in), static_cast<usize>(n)); }

    void put_text(std::string_view s)
    {
        std::memcpy(b + text, s.data(), s.size());
        b[text + s.size()] = 0;
    }

    // The fd sets at `sets`: read, write, except.
    u32 rset() const { return sets; }
    u32 wset() const { return sets + 128; }
    u32 eset() const { return sets + 256; }
    void clear_sets() { std::memset(b + sets, 0, 384); }
    // Descriptor d is bit d % 32 of big-endian word d / 32.
    void add(u32 set, s32 fd)
    {
        const u32 word = set + 4 * static_cast<u32>(fd / 32);
        ps_write32(b, word, ps_read32(b, word) | 1u << (fd % 32));
    }
    bool has(u32 set, s32 fd) const { return (ps_read32(b, set + 4 * static_cast<u32>(fd / 32)) >> (fd % 32)) & 1; }
    bool empty(u32 set) const
    {
        for (u32 i = 0; i < 128; i++)
            if (b[set + i]) return false;
        return true;
    }
    Outcome select(s32 nfds, std::optional<std::pair<s64, s64>> timeout)
    {
        if (timeout) {
            ps_write64(b, tv, static_cast<u64>(timeout->first));
            ps_write64(b, tv + 8, static_cast<u64>(timeout->second));
        }
        return call("socketselect", nfds, rset(), wset(), eset(), timeout ? tv : 0);
    }
};

// Waits (up to 10 s) until the guest thread published in `t` is parked in a sys_net wait.
bool wait_parked(const std::atomic<lv2::PpuThread*>& t)
{
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < until) {
        if (lv2::PpuThread* p = t.load(); p && p->waiter.waiting_on().kind == lv2::WaitKind::Net) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool all_bytes(const u8* p, usize n, u8 value)
{
    for (usize i = 0; i < n; i++)
        if (p[i] != value) return false;
    return true;
}

} // namespace

TEST_CASE("sys_net, cellHttp, cellSsl: the functions GTA V imports, in their modules", "[hle][net]")
{
    struct Expected {
        std::string_view module, name;
        u32 nid;
    };
    const Expected expected[] = {
        {"sys_net", "getsockname", 0x13EFE7F5},
        {"sys_net", "recvfrom", 0x1F953B9F},
        {"sys_net", "listen", 0x28E208BB},
        {"sys_net", "sys_net_get_sockinfo", 0x3B27C780},
        {"sys_net", "socketselect", 0x3F09E20A},
        {"sys_net", "_sys_net_errno_loc", 0x6005CDE1},
        {"sys_net", "connect", 0x64F66D35},
        {"sys_net", "socketclose", 0x6DB6E8CD},
        {"sys_net", "gethostbyname", 0x71F4C717},
        {"sys_net", "sys_net_set_resolver_configurations", 0x7687D48C},
        {"sys_net", "setsockopt", 0x88F03575},
        {"sys_net", "inet_pton", 0x8AF3825E},
        {"sys_net", "sendto", 0x9647570B},
        {"sys_net", "socket", 0x9C056962},
        {"sys_net", "shutdown", 0xA50777C6},
        {"sys_net", "sys_net_get_sockinfo_ex", 0xA765D029},
        {"sys_net", "bind", 0xB0A59804},
        {"sys_net", "sys_net_finalize_network", 0xB68D5625},
        {"sys_net", "accept", 0xC94F6939},
        {"sys_net", "inet_ntop", 0xC98A3146},
        {"sys_net", "inet_addr", 0xDABBC2C0},
        {"sys_net", "send", 0xDC751B40},
        {"sys_net", "recv", 0xFBA04F37},
        {"cellHttp", "cellHttpInit", 0x250C386C},
        {"cellHttp", "cellHttpsInit", 0x522180BC},
        {"cellHttp", "cellHttpEndCookie", 0x61B2BADE},
        {"cellHttp", "cellHttpInitCookie", 0x9638F766},
        {"cellHttp", "cellHttpEnd", 0xD276FF1F},
        {"cellHttp", "cellHttpsEnd", 0xE6D4202F},
        {"cellSsl", "cellSslEnd", 0x1650AEA4},
        {"cellSsl", "cellSslCertificateLoader", 0x571AFACA},
        {"cellSsl", "cellSslInit", 0xFB02C9D2},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const Expected& e : expected) {
        INFO(e.module << "::" << e.name);
        const hle::Function* fn = hle::registry().find(e.module, e.nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == e.name);
    }
}

TEST_CASE("sys_net sockets: descriptors from 24 in order, the type checks, socketclose, EMFILE", "[hle][net]")
{
    Fixture f;
    GuestThread g(f, 0, "net");
    Net n(g, 0);

    CHECK(n.call("socket", kAfInet, kStream, 0) == ok(24));
    CHECK(n.call("socket", kAfInet, kDgram, kIpprotoUdp) == ok(25));
    CHECK(n.call("socket", kAfInet, kStream, kIpprotoTcp) == ok(26));
    CHECK(n.call("socketclose", 25) == ok());
    CHECK(n.call("socketclose", 25) == fail(SYS_NET_EBADF));
    CHECK(n.call("socket", kAfInet, kDgram, 0) == ok(27)); // the next one, not the freed 25 (RPCS3's IdManager)

    // A failure returns SYS_NET_ERROR_* (sign-extended in r3) and leaves the code in sys_net_errno.
    CHECK(g.call("socket", kAfInet, 4, 0) == r3_of_code(0x8001022B));
    CHECK(n.call("socket", kAfInet, 4, 0) == fail(SYS_NET_EPROTONOSUPPORT)); // the type first
    CHECK(n.call("socket", kAfInet6, 4, 0) == fail(SYS_NET_EPROTONOSUPPORT));
    CHECK(n.call("socket", kAfInet, kStream, kIpprotoUdp) == fail(SYS_NET_EPROTONOSUPPORT));
    CHECK(n.call("socket", kAfInet, kDgram, kIpprotoTcp) == fail(SYS_NET_EPROTONOSUPPORT));
    CHECK(n.call("socket", kAfInet6, kStream, 0) == fail(SYS_NET_EAFNOSUPPORT));
    CHECK(n.call("socket", kAfInet, kRaw, 1) == ok(28));
    CHECK(n.call("socket", kAfInet, kDgramP2p, 0) == ok(29));
    CHECK(n.call("socket", kAfInet, kStreamP2p, 0) == ok(30));
    for (const s32 bad : {-1, 0, 23, 31, 1024, 0x7FFFFFFF}) {
        INFO("socket " << bad);
        CHECK(n.call("socketclose", bad) == fail(SYS_NET_EBADF));
    }

    // 1000 descriptors, up to 1023; then the lowest free ones are handed out again (25 first, closed above).
    for (s32 id = 31; id <= 1023; id++) REQUIRE(n.call("socket", kAfInet, kDgram, 0) == ok(id));
    CHECK(n.call("socket", kAfInet, kDgram, 0) == ok(25));
    CHECK(n.call("socket", kAfInet, kDgram, 0) == fail(SYS_NET_EMFILE));
    REQUIRE(n.call("socketclose", 600) == ok());
    REQUIRE(n.call("socketclose", 40) == ok());
    CHECK(n.call("socket", kAfInet, kStream, 0) == ok(40));
    CHECK(n.call("socket", kAfInet, kStream, 0) == ok(600));
    CHECK(n.call("socket", kAfInet, kStream, 0) == fail(SYS_NET_EMFILE));
}

TEST_CASE("sys_net_errno: in the thread's context in libnet's pool, set only by a failing call", "[hle][net]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread g(f, 0, "net");

    // Before sys_net_initialize_network_ex: libnet's static context, aborted with EBUSY; lv2 is not asked.
    REQUIRE(f.hle->link_internal_functions({"sys_net"}));
    CHECK(g.call("socket", kAfInet, kDgram, 0) == r3_of_code(0x80010210));
    const u32 static_errno = static_cast<u32>(g.call("_sys_net_errno_loc"));
    CHECK(ps_read32(b, static_errno) == 16);

    Net n(g, 0); // initialises libnet
    const u32 loc = static_cast<u32>(g.call("_sys_net_errno_loc"));
    CHECK(loc >= kNetPool);
    CHECK(loc < kNetPool + 0x80000);
    CHECK(g.call("_sys_net_h_errno_loc") == loc - 28);
    CHECK(n.call("socketclose", 77) == fail(SYS_NET_EBADF));
    CHECK(g.call("socketclose", 77) == r3_of_code(0x80010209)); // SYS_NET_ERROR_EBADF, not -1
    CHECK(ps_read32(b, loc) == SYS_NET_EBADF);
    CHECK(n.call("socket", kAfInet, kDgram, 0) == ok(24));
    CHECK(ps_read32(b, loc) == SYS_NET_EBADF); // a success leaves it alone
    CHECK(n.call("listen", 24, 0) == fail(SYS_NET_EINVAL));

    // Each thread its own context.
    GuestThread h(f, 1, "second");
    const u64 hloc = h.call("_sys_net_errno_loc");
    CHECK(hloc != loc);
    CHECK(h.call("_sys_net_errno_loc") == hloc);
    CHECK(h.call("socketclose", 99) == r3_of_code(0x80010209));
    CHECK(ps_read32(b, static_cast<u32>(hloc)) == SYS_NET_EBADF);
    CHECK(ps_read32(b, loc) == SYS_NET_EINVAL);

    // sys_net_free_thread_context(0, 1) frees the caller's; its next call makes a new one.
    CHECK(h.call("sys_net_free_thread_context", 0, 1) == 1);
    CHECK(g.call("sys_net_finalize_network") == 0);
    CHECK(g.call("socket", kAfInet, kDgram, 0) == r3_of_code(0x80010210));
}

TEST_CASE("sys_net bind, listen, getsockname: RPCS3's checks, the loopback interface the only one", "[hle][net]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread g(f, 0, "net");
    Net n(g, 0);
    const s32 u = n.socket(kDgram);

    // getsockname before bind: 0.0.0.0:0 (16 bytes, AF_INET); the length comes back 16.
    std::memset(b + n.addr2, 0xAA, 16);
    ps_write32(b, n.len, 64);
    REQUIRE(n.call("getsockname", u, n.addr2, n.len) == ok());
    CHECK(ps_read8(b, n.addr2) == 16);
    CHECK(ps_read8(b, n.addr2 + 1) == kAfInet);
    CHECK(all_bytes(b + n.addr2 + 2, 14, 0));
    CHECK(ps_read32(b, n.len) == 16);
    CHECK(n.call("getsockname", u, 0, n.len) == fail(SYS_NET_EINVAL));
    CHECK(n.call("getsockname", u, n.addr2, 0) == fail(SYS_NET_EINVAL));
    ps_write32(b, n.len, 15);
    CHECK(n.call("getsockname", u, n.addr2, n.len) == fail(SYS_NET_EINVAL));
    ps_write32(b, n.len, 16);
    CHECK(n.call("getsockname", 99, n.addr2, n.len) == fail(SYS_NET_EBADF));
    CHECK(n.call("getsockname", u, kUnmapped, n.len) == fail(SYS_NET_EFAULT)); // RPCS3 faults
    CHECK(n.call("getsockname", u, n.addr2, kUnmapped) == fail(SYS_NET_EFAULT));

    // bind: the address (pointer, length), the socket, then the family.
    CHECK(n.call("bind", u, 0, 16) == fail(SYS_NET_EINVAL));
    n.put_addr(n.addr, 0, 6672);
    CHECK(n.call("bind", u, n.addr, 15) == fail(SYS_NET_EINVAL));
    CHECK(n.call("bind", 99, n.addr, 16) == fail(SYS_NET_EBADF));
    CHECK(n.call("bind", u, kUnmapped, 16) == fail(SYS_NET_EFAULT));
    n.put_addr(n.addr, 0, 6672, 0, kAfInet6);
    CHECK(n.call("bind", 99, n.addr, 16) == fail(SYS_NET_EBADF));
    CHECK(n.call("bind", u, n.addr, 16) == fail(SYS_NET_EAFNOSUPPORT));
    // Without a link no interface has another address.
    CHECK(n.bind(u, 0xC0A80002, 6672) == fail(SYS_NET_EADDRNOTAVAIL)); // 192.168.0.2
    CHECK(n.bind(u, 0x7F000002, 6672) == fail(SYS_NET_EADDRNOTAVAIL)); // 127.0.0.2 isn't configured
    // GTA V's game port, family AF_UNSPEC (taken as AF_INET, as RPCS3).
    n.put_addr(n.addr, 0, 6672, 0, 0);
    CHECK(n.call("bind", u, n.addr, 16) == ok());
    CHECK(n.name_of(u) == std::pair<u16, u32>{6672, 0});
    CHECK(n.bind(u, 0, 6673) == fail(SYS_NET_EINVAL)); // bound already

    // A port in use: EADDRINUSE unless both sockets allow reuse.
    const s32 u2 = n.socket(kDgram);
    CHECK(n.bind(u2, kLoopback, 6672) == fail(SYS_NET_EADDRINUSE));
    CHECK(n.setopt(u2, kSolSocket, kSoReuseaddr, 1) == ok());
    CHECK(n.bind(u2, kLoopback, 6672) == fail(SYS_NET_EADDRINUSE));
    CHECK(n.setopt(u, kSolSocket, kSoReuseport, 1) == ok());
    CHECK(n.bind(u2, kLoopback, 6672) == ok());
    // Stream sockets have ports of their own; port 0 picks an ephemeral one.
    const s32 t = n.socket(kStream);
    CHECK(n.bind(t, kLoopback, 6672) == ok());
    const s32 e = n.socket(kStream);
    CHECK(n.bind(e, kLoopback, 0) == ok());
    const auto [port, ip] = n.name_of(e);
    CHECK(port >= 49152);
    CHECK(ip == kLoopback);
    // A multicast group: datagram sockets only.
    CHECK(n.bind(n.socket(kDgram), 0xE0000001, 5000) == ok());
    CHECK(n.bind(n.socket(kStream), 0xE0000001, 5000) == fail(SYS_NET_EADDRNOTAVAIL));
    // Raw sockets: RPCS3's stub accepts any bind and keeps no address.
    const s32 r = n.socket(kRaw, 1);
    CHECK(n.bind(r, 0x08080808, 1) == ok());
    std::memset(b + n.addr2, 0xAA, 16);
    ps_write32(b, n.len, 16);
    REQUIRE(n.call("getsockname", r, n.addr2, n.len) == ok());
    CHECK(all_bytes(b + n.addr2, 16, 0));

    // listen: a positive backlog, then the socket; datagram sockets can't; an unbound stream socket gets a port.
    CHECK(n.call("listen", t, 0) == fail(SYS_NET_EINVAL));
    CHECK(n.call("listen", t, -1) == fail(SYS_NET_EINVAL));
    CHECK(n.call("listen", 99, 0) == fail(SYS_NET_EINVAL));
    CHECK(n.call("listen", 99, 5) == fail(SYS_NET_EBADF));
    CHECK(n.call("listen", u, 5) == fail(SYS_NET_EOPNOTSUPP));
    CHECK(n.call("listen", t, 5) == ok());
    CHECK(n.call("listen", t, 10) == ok());
    const s32 l = n.socket(kStream);
    CHECK(n.call("listen", l, 1) == ok());
    CHECK(n.name_of(l).first >= 49152);
    CHECK(n.call("listen", n.socket(kDgramP2p), 1) == fail(SYS_NET_EOPNOTSUPP)); // RPCS3 aborts
    CHECK(n.call("listen", n.socket(kStreamP2p), 1) == ok());
    CHECK(n.call("listen", r, 1) == ok()); // RPCS3's stub
}

TEST_CASE("sys_net P2P sockets: bound by virtual port, getsockname as RPCS3 lays it out", "[hle][net]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread g(f, 0, "net");
    Net n(g, 0);
    const s32 p = n.socket(kDgramP2p);

    std::memset(b + n.addr2, 0xAA, 16);
    ps_write32(b, n.len, 16);
    REQUIRE(n.call("getsockname", p, n.addr2, n.len) == ok());
    CHECK(all_bytes(b + n.addr2, 16, 0)); // unbound: all zero

    CHECK(n.bind(p, 0, 0) == fail(SYS_NET_EINVAL)); // a UDP port is needed
    CHECK(n.bind(p, 0, 3658) == ok());               // virtual port 0: the first free one from 30000
    REQUIRE(n.call("getsockname", p, n.addr2, n.len) == ok());
    CHECK(ps_read8(b, n.addr2) == 16);
    CHECK(ps_read8(b, n.addr2 + 1) == kAfInet);
    CHECK(ps_read16(b, n.addr2 + 2) == 3658);
    CHECK(ps_read32(b, n.addr2 + 4) == 0);
    CHECK(ps_read16(b, n.addr2 + 8) == 30000);
    CHECK(all_bytes(b + n.addr2 + 10, 6, 0));

    const s32 q = n.socket(kDgramP2p);
    CHECK(n.bind(q, 0, 3658, 30000) == fail(SYS_NET_EADDRINUSE));
    CHECK(n.bind(q, 0, 3658) == ok());
    REQUIRE(n.call("getsockname", q, n.addr2, n.len) == ok());
    CHECK(ps_read16(b, n.addr2 + 8) == 30001);
    CHECK(n.bind(n.socket(kDgramP2p), 0, 3659, 30000) == ok()); // another UDP port
    // A virtual port in use takes another socket when every socket on it allows reuse (RPCS3 all_reusable).
    CHECK(n.setopt(p, kSolSocket, kSoReuseaddr, 1) == ok());
    CHECK(n.bind(n.socket(kDgramP2p), 0, 3658, 30000) == ok());

    // A stream P2P socket: sin_port is the virtual port, sin_vport the UDP port (3658 when 0), and getsockname
    // reports them the same way round.
    const s32 sp = n.socket(kStreamP2p);
    CHECK(n.bind(sp, 0, 5) == ok());
    REQUIRE(n.call("getsockname", sp, n.addr2, n.len) == ok());
    CHECK(ps_read16(b, n.addr2 + 2) == 5);
    CHECK(ps_read16(b, n.addr2 + 8) == 3658);

    // P2P sockets keep any option (RPCS3).
    CHECK(n.setopt(p, 0x1234, 0x5678, 1) == ok());
    CHECK(n.setopt(p, kSolSocket, 0x7777, 1) == ok());
    CHECK(n.call("setsockopt", p, kSolSocket, kSoNbio, n.opt, 3) == fail(SYS_NET_EINVAL)); // the length still counts
}

TEST_CASE("sys_net setsockopt: RPCS3's options and checks", "[hle][net]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread g(f, 0, "net");
    Net n(g, 0);
    const s32 u = n.socket(kDgram), t = n.socket(kStream);

    ps_write32(b, n.opt, 1);
    CHECK(n.call("setsockopt", u, kSolSocket, kSoNbio, n.opt, 3) == fail(SYS_NET_EINVAL));
    CHECK(n.call("setsockopt", 99, kSolSocket, kSoNbio, n.opt, 3) == fail(SYS_NET_EINVAL)); // the length first
    CHECK(n.call("setsockopt", 99, kSolSocket, kSoNbio, n.opt, 4) == fail(SYS_NET_EBADF));
    CHECK(n.call("setsockopt", u, kSolSocket, kSoNbio, kUnmapped, 4) == fail(SYS_NET_EFAULT)); // RPCS3 faults
    for (const s32 opt : {kSoNbio, kSoReuseaddr, kSoReuseport, kSoBroadcast, kSoKeepalive, kSoSndbuf, kSoRcvbuf,
             kSoSndlowat, kSoRcvlowat, kSoUsecrypto, kSoUsesignature}) {
        INFO("option " << opt);
        CHECK(n.setopt(u, kSolSocket, opt, 1) == ok());
    }
    CHECK(n.setopt(u, kSolSocket, kSoRcvbuf, 0) == fail(SYS_NET_EINVAL));
    CHECK(n.setopt(u, kSolSocket, kSoSndlowat, -1) == fail(SYS_NET_EINVAL));
    CHECK(n.setopt(u, kSolSocket, 0x1007, 1) == fail(SYS_NET_EINVAL)); // SO_ERROR: not settable
    CHECK(n.setopt(u, kSolSocket, 0x0100, 1) == fail(SYS_NET_EINVAL)); // SO_OOBINLINE: not in RPCS3's list
    CHECK(n.setopt(u, 0x1234, 1, 1) == fail(SYS_NET_EINVAL));         // an unknown level

    // The timeouts: a 16-byte sys_net_timeval, normalised.
    CHECK(n.call("setsockopt", u, kSolSocket, kSoRcvtimeo, n.opt, 8) == fail(SYS_NET_EINVAL));
    CHECK(n.set_timeout(u, kSoRcvtimeo, 1, 500000) == ok());
    CHECK(n.set_timeout(u, kSoSndtimeo, 0, 0) == ok());
    CHECK(n.set_timeout(u, kSoRcvtimeo, -1, 0) == fail(SYS_NET_EDOM));
    CHECK(n.set_timeout(u, kSoRcvtimeo, 0, 1000000) == fail(SYS_NET_EDOM));
    CHECK(n.set_timeout(u, kSoRcvtimeo, 0, -1) == fail(SYS_NET_EDOM));
    // SO_LINGER: an 8-byte sys_net_linger.
    CHECK(n.call("setsockopt", t, kSolSocket, kSoLinger, n.opt, 4) == fail(SYS_NET_EINVAL));
    CHECK(n.call("setsockopt", t, kSolSocket, kSoLinger, n.opt, 8) == ok());

    // TCP: MAXSEG anywhere (RPCS3 keeps it), NODELAY on a stream socket.
    CHECK(n.setopt(t, kIpprotoTcp, kTcpNodelay, 1) == ok());
    CHECK(n.setopt(u, kIpprotoTcp, kTcpNodelay, 1) == fail(SYS_NET_EINVAL));
    CHECK(n.setopt(u, kIpprotoTcp, kTcpMaxseg, 1400) == ok());
    CHECK(n.setopt(t, kIpprotoTcp, 3, 1) == fail(SYS_NET_EINVAL));

    // IP.
    for (const s32 opt : {kIpTos, kIpTtl, kIpMulticastTtl, kIpMulticastLoop, kIpTtlchk, kIpMaxttl, kIpDontfrag}) {
        INFO("option " << opt);
        CHECK(n.setopt(u, kIpprotoIp, opt, 1) == ok());
    }
    CHECK(n.setopt(u, kIpprotoIp, 99, 1) == fail(SYS_NET_EINVAL));
    // Multicast groups (struct ip_mreq): with no link only the loopback interface can join one.
    ps_write32(b, n.opt, 0xE0000001);
    ps_write32(b, n.opt + 4, 0);
    CHECK(n.call("setsockopt", u, kIpprotoIp, kIpAddMembership, n.opt, 4) == fail(SYS_NET_EINVAL));
    CHECK(n.call("setsockopt", u, kIpprotoIp, kIpAddMembership, n.opt, 8) == fail(SYS_NET_EADDRNOTAVAIL));
    ps_write32(b, n.opt + 4, kLoopback);
    CHECK(n.call("setsockopt", u, kIpprotoIp, kIpAddMembership, n.opt, 8) == ok());
    CHECK(n.call("setsockopt", u, kIpprotoIp, kIpAddMembership, n.opt, 8) == fail(SYS_NET_EADDRINUSE));
    CHECK(n.call("setsockopt", u, kIpprotoIp, kIpDropMembership, n.opt, 8) == ok());
    CHECK(n.call("setsockopt", u, kIpprotoIp, kIpDropMembership, n.opt, 8) == fail(SYS_NET_EADDRNOTAVAIL));
    CHECK(n.call("setsockopt", t, kIpprotoIp, kIpAddMembership, n.opt, 8) == fail(SYS_NET_EOPNOTSUPP));
    ps_write32(b, n.opt, 0x01020304);
    CHECK(n.call("setsockopt", u, kIpprotoIp, kIpAddMembership, n.opt, 8) == fail(SYS_NET_EINVAL)); // not a group
}

TEST_CASE("sys_net without a network: connect and sendto anywhere but the loopback fail with EADDRNOTAVAIL",
    "[hle][net]")
{
    Fixture f;
    GuestThread g(f, 0, "net");
    Net n(g, 0);
    const s32 t = n.socket(kStream), u = n.socket(kDgram);

    for (const u32 ip : {0x08080808u, 0xC0A80001u, 0x0A000001u, 0xAC100001u, 0xFFFFFFFFu, 0xE0000001u}) {
        INFO("address " << std::hex << ip);
        CHECK(n.connect(t, ip, 80) == fail(SYS_NET_EADDRNOTAVAIL));
        CHECK(n.connect(u, ip, 53) == fail(SYS_NET_EADDRNOTAVAIL));
        CHECK(n.sendto(u, "query", ip, 53) == fail(SYS_NET_EADDRNOTAVAIL));
        CHECK(n.sendto(t, "data", ip, 80) == fail(SYS_NET_EADDRNOTAVAIL));
    }
    CHECK(n.name_of(u) == std::pair<u16, u32>{0, 0}); // nothing was sent: still unbound

    // connect: the address and its family, then the socket.
    CHECK(n.call("connect", t, 0, 16) == fail(SYS_NET_EINVAL));
    n.put_addr(n.addr, 0x08080808, 80);
    CHECK(n.call("connect", t, n.addr, 8) == fail(SYS_NET_EINVAL));
    CHECK(n.call("connect", t, kUnmapped, 16) == fail(SYS_NET_EFAULT));
    n.put_addr(n.addr, 0x08080808, 80, 0, 0);
    CHECK(n.call("connect", 99, n.addr, 16) == fail(SYS_NET_EAFNOSUPPORT));
    CHECK(n.connect(99, 0x08080808, 80) == fail(SYS_NET_EBADF));

    // Nothing listens on the loopback port; nothing is connected.
    CHECK(n.connect(t, kLoopback, 80) == fail(SYS_NET_ECONNREFUSED));
    CHECK(n.connect(t, kLoopback, 0) == fail(SYS_NET_EADDRNOTAVAIL));
    CHECK(n.send(t, "data") == fail(SYS_NET_ENOTCONN));
    CHECK(n.send(u, "data") == fail(SYS_NET_EDESTADDRREQ));
    CHECK(n.call("shutdown", t, 2) == fail(SYS_NET_ENOTCONN));
    CHECK(n.call("shutdown", u, 0) == fail(SYS_NET_ENOTCONN));
    CHECK(n.call("shutdown", t, 3) == fail(SYS_NET_EINVAL));
    CHECK(n.call("shutdown", t, -1) == fail(SYS_NET_EINVAL));
    CHECK(n.call("shutdown", 99, 0) == fail(SYS_NET_EBADF));

    // sendto's checks: the flags (RPCS3 aborts on others), the address, the buffer, then the socket.
    CHECK(n.sendto(u, "x", kLoopback, 9, 0x1) == fail(SYS_NET_EINVAL)); // MSG_OOB
    CHECK(n.call("sendto", u, n.out, 1, 0, n.addr, 15) == fail(SYS_NET_EINVAL));
    n.put_addr(n.addr, kLoopback, 9, 0, kAfInet6);
    CHECK(n.call("sendto", u, n.out, 1, 0, n.addr, 16) == fail(SYS_NET_EAFNOSUPPORT));
    n.put_addr(n.addr, kLoopback, 9);
    CHECK(n.call("sendto", u, kUnmapped, 1, 0, n.addr, 16) == fail(SYS_NET_EFAULT));
    CHECK(n.call("sendto", 99, n.out, 1, 0, n.addr, 16) == fail(SYS_NET_EBADF));

    // P2P: a broadcast needs SO_BROADCAST (RPCS3); nothing is reachable either way.
    const s32 p = n.socket(kDgramP2p);
    CHECK(n.sendto(p, "p2p", kLoopback, 3658, 0, 1) == fail(SYS_NET_EINVAL)); // not bound (RPCS3 asserts)
    REQUIRE(n.bind(p, 0, 3658) == ok());
    CHECK(n.sendto(p, "p2p", 0xFFFFFFFF, 3658, 0, 1) == fail(SYS_NET_EACCES));
    REQUIRE(n.setopt(p, kSolSocket, kSoBroadcast, 1) == ok());
    CHECK(n.sendto(p, "p2p", 0xFFFFFFFF, 3658, 0, 1) == fail(SYS_NET_EADDRNOTAVAIL));
    CHECK(n.sendto(p, "p2p", 0x08080808, 3658, 0, 1) == fail(SYS_NET_EADDRNOTAVAIL));
    CHECK(n.send(p, "p2p") == fail(SYS_NET_EDESTADDRREQ));
    CHECK(n.connect(p, kLoopback, 3658) == fail(SYS_NET_EOPNOTSUPP)); // RPCS3 aborts
    // A P2P stream connect: the virtual port is sin_port; no handshake is ever answered.
    const s32 sp = n.socket(kStreamP2p);
    CHECK(n.connect(sp, 0x08080808, 0, 3658) == fail(SYS_NET_EADDRNOTAVAIL));
    CHECK(n.connect(sp, 0x08080808, 1, 3658) == fail(SYS_NET_EADDRNOTAVAIL));
    CHECK(n.connect(sp, kLoopback, 1, 3658) == fail(SYS_NET_ECONNREFUSED));
    CHECK(n.send(sp, "x") == fail(SYS_NET_ENOTCONN));
    // Raw sockets: RPCS3's stub takes what is sent to the loopback; elsewhere is unreachable.
    const s32 r = n.socket(kRaw, 1);
    CHECK(n.sendto(r, "ping", kLoopback, 0) == ok(4));
    CHECK(n.sendto(r, "ping", 0x08080808, 0) == fail(SYS_NET_EADDRNOTAVAIL));
    CHECK(n.connect(r, kLoopback, 1) == fail(SYS_NET_EOPNOTSUPP)); // RPCS3 aborts
}

TEST_CASE("sys_net receiving without a network: EWOULDBLOCK on non-blocking sockets, RPCS3's checks", "[hle][net]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread g(f, 0, "net");
    Net n(g, 0);
    const s32 u = n.socket(kDgram);
    REQUIRE(n.bind(u, 0, 6672) == ok());
    REQUIRE(n.setopt(u, kSolSocket, kSoNbio, 1) == ok());
    CHECK(n.recvfrom(u, 0x100) == fail(SYS_NET_EWOULDBLOCK));
    CHECK(ps_read32(b, n.len) == 0xAAAAAAAA); // nothing written
    CHECK(n.recv(u, 0x100) == fail(SYS_NET_EWOULDBLOCK));
    const s32 v = n.socket(kDgram); // blocking, MSG_DONTWAIT
    CHECK(n.recv(v, 0x100, kMsgDontwait) == fail(SYS_NET_EWOULDBLOCK));

    // recvfrom: a buffer and a length, addr and paddrlen both or neither, known flags, then the socket.
    CHECK(n.call("recvfrom", u, 0, 16, 0, 0, 0) == fail(SYS_NET_EINVAL));
    CHECK(n.call("recvfrom", u, n.in, 0, 0, 0, 0) == fail(SYS_NET_EINVAL));
    CHECK(n.call("recvfrom", u, n.in, 16, 0, n.addr2, 0) == fail(SYS_NET_EINVAL));
    CHECK(n.call("recvfrom", u, n.in, 16, 0, 0, n.len) == fail(SYS_NET_EINVAL));
    CHECK(n.call("recvfrom", u, n.in, 16, 0x1, 0, 0) == fail(SYS_NET_EINVAL)); // MSG_OOB: RPCS3 aborts
    CHECK(n.call("recvfrom", 99, n.in, 16, 0, 0, 0) == fail(SYS_NET_EBADF));
    CHECK(n.call("recvfrom", u, n.in, 16, 0, kUnmapped, n.len) == fail(SYS_NET_EFAULT));

    // A stream socket that isn't connected: ENOTCONN at once, blocking or not, listening or not.
    const s32 t = n.socket(kStream);
    CHECK(n.recv(t, 16) == fail(SYS_NET_ENOTCONN));
    REQUIRE(n.call("listen", t, 4) == ok());
    CHECK(n.recv(t, 16) == fail(SYS_NET_ENOTCONN));

    // accept: addr and paddrlen both or neither, a length of at least 16, then the socket.
    REQUIRE(n.setopt(t, kSolSocket, kSoNbio, 1) == ok());
    CHECK(n.call("accept", t, 0, 0) == fail(SYS_NET_EWOULDBLOCK));
    ps_write32(b, n.len, 16);
    CHECK(n.call("accept", t, n.addr2, n.len) == fail(SYS_NET_EWOULDBLOCK));
    CHECK(n.call("accept", t, n.addr2, 0) == fail(SYS_NET_EINVAL));
    ps_write32(b, n.len, 8);
    CHECK(n.call("accept", t, n.addr2, n.len) == fail(SYS_NET_EINVAL));
    CHECK(n.call("accept", t, n.addr2, kUnmapped) == fail(SYS_NET_EFAULT));
    CHECK(n.call("accept", 99, 0, 0) == fail(SYS_NET_EBADF));
    CHECK(n.call("accept", u, 0, 0) == fail(SYS_NET_EOPNOTSUPP));
    CHECK(n.call("accept", n.socket(kStream), 0, 0) == fail(SYS_NET_EINVAL)); // not listening
    CHECK(n.call("accept", n.socket(kDgramP2p), 0, 0) == fail(SYS_NET_EOPNOTSUPP)); // RPCS3 aborts

    // P2P and raw sockets would block too; a P2P stream socket that doesn't listen isn't connected.
    const s32 p = n.socket(kDgramP2p);
    REQUIRE(n.setopt(p, kSolSocket, kSoNbio, 1) == ok());
    CHECK(n.recvfrom(p, 16) == fail(SYS_NET_EWOULDBLOCK));
    CHECK(n.recv(n.socket(kRaw, 1), 16, kMsgDontwait) == fail(SYS_NET_EWOULDBLOCK));
    const s32 sp = n.socket(kStreamP2p);
    CHECK(n.recv(sp, 16) == fail(SYS_NET_ENOTCONN));
    REQUIRE(n.call("listen", sp, 1) == ok());
    CHECK(n.recv(sp, 16, kMsgDontwait) == fail(SYS_NET_EWOULDBLOCK));
}

TEST_CASE("sys_net blocking receive: SO_RCVTIMEO, loopback data and socketclose end the wait", "[hle][net]")
{
    Fixture f;
    GuestThread g(f, 0, "net");
    Net n(g, 0);
    const s32 u = n.socket(kDgram);
    REQUIRE(n.bind(u, kLoopback, 7000) == ok());

    // SO_RCVTIMEO: EWOULDBLOCK once it has passed (RPCS3); accept honours it too.
    REQUIRE(n.set_timeout(u, kSoRcvtimeo, 0, 50000) == ok());
    auto t0 = std::chrono::steady_clock::now();
    CHECK(n.recvfrom(u, 64) == fail(SYS_NET_EWOULDBLOCK));
    CHECK(std::chrono::steady_clock::now() - t0 >= 50ms);
    const s32 l = n.socket(kStream);
    REQUIRE(n.bind(l, kLoopback, 7001) == ok());
    REQUIRE(n.call("listen", l, 1) == ok());
    REQUIRE(n.set_timeout(l, kSoRcvtimeo, 0, 30000) == ok());
    t0 = std::chrono::steady_clock::now();
    CHECK(n.call("accept", l, 0, 0) == fail(SYS_NET_EWOULDBLOCK));
    CHECK(std::chrono::steady_clock::now() - t0 >= 30ms);
    REQUIRE(n.set_timeout(u, kSoRcvtimeo, 0, 0) == ok()); // no timeout again

    // Without a timeout a blocking recvfrom waits, here for a loopback datagram another thread sends.
    std::atomic<lv2::PpuThread*> parked{nullptr};
    Outcome got;
    std::string text;
    u16 from_port = 0;
    u32 from_addr = 0;
    std::thread receiver([&] {
        GuestThread r(f, 1, "receiver");
        Net rn(r, 1);
        parked = r.t;
        got = rn.recvfrom(u, 64);
        if (got.value > 0) text = rn.received(got.value);
        from_port = ps_read16(rn.b, rn.addr2 + 2);
        from_addr = ps_read32(rn.b, rn.addr2 + 4);
        parked = nullptr;
    });
    REQUIRE(wait_parked(parked));
    const s32 sender = n.socket(kDgram);
    REQUIRE(n.sendto(sender, "wake up", kLoopback, 7000) == ok(7));
    receiver.join();
    CHECK(got == ok(7));
    CHECK(text == "wake up");
    CHECK(from_addr == kLoopback);
    CHECK(from_port == n.name_of(sender).first);

    // A socketclose from another thread ends the wait with EINTR (RPCS3 abort_socket).
    std::thread receiver2([&] {
        GuestThread r(f, 2, "receiver 2");
        Net rn(r, 2);
        parked = r.t;
        got = rn.recv(u, 64);
        parked = nullptr;
    });
    REQUIRE(wait_parked(parked));
    REQUIRE(n.call("socketclose", u) == ok());
    receiver2.join();
    CHECK(got == fail(SYS_NET_EINTR));

    // ... and a blocked accept the same way.
    REQUIRE(n.set_timeout(l, kSoRcvtimeo, 0, 0) == ok());
    std::thread acceptor([&] {
        GuestThread r(f, 3, "acceptor");
        Net rn(r, 3);
        parked = r.t;
        got = rn.call("accept", l, 0, 0);
        parked = nullptr;
    });
    REQUIRE(wait_parked(parked));
    REQUIRE(n.call("socketclose", l) == ok());
    acceptor.join();
    CHECK(got == fail(SYS_NET_EINTR));
}

TEST_CASE("sys_net loopback UDP: datagrams between the game's own sockets", "[hle][net]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread g(f, 0, "net");
    Net n(g, 0);
    const s32 a = n.socket(kDgram), c = n.socket(kDgram);
    REQUIRE(n.bind(a, kLoopback, 5000) == ok());
    REQUIRE(n.setopt(a, kSolSocket, kSoNbio, 1) == ok());

    // The sender is bound implicitly, to an ephemeral port; the receiver sees 127.0.0.1 and that port.
    CHECK(n.sendto(c, "hello", kLoopback, 5000) == ok(5));
    const auto [cport, caddr] = n.name_of(c);
    CHECK(cport >= 49152);
    CHECK(caddr == 0);
    CHECK(n.recvfrom(a, 64) == ok(5));
    CHECK(n.received(5) == "hello");
    CHECK(ps_read8(b, n.addr2) == 16);
    CHECK(ps_read8(b, n.addr2 + 1) == kAfInet);
    CHECK(ps_read16(b, n.addr2 + 2) == cport);
    CHECK(ps_read32(b, n.addr2 + 4) == kLoopback);
    CHECK(all_bytes(b + n.addr2 + 8, 8, 0));
    CHECK(ps_read32(b, n.len) == 16);

    // 0.0.0.0 as a destination is the loopback address too (BSD); nobody bound there: dropped, sent all the same.
    CHECK(n.sendto(c, "any", 0, 5000) == ok(3));
    CHECK(n.recv(a, 64) == ok(3));
    CHECK(n.sendto(c, "lost", kLoopback, 5001) == ok(4));
    CHECK(n.sendto(c, "lost", kLoopback, 0) == fail(SYS_NET_EADDRNOTAVAIL)); // port 0 is no destination

    // A datagram cut to the length read (the rest goes), MSG_PEEK leaves it, an empty one reads 0 with a zero address.
    CHECK(n.sendto(c, "0123456789", kLoopback, 5000) == ok(10));
    CHECK(n.recv(a, 4, kMsgPeek) == ok(4));
    CHECK(n.received(4) == "0123");
    CHECK(n.recv(a, 4) == ok(4));
    CHECK(n.received(4) == "0123");
    CHECK(n.recv(a, 64) == fail(SYS_NET_EWOULDBLOCK));
    CHECK(n.sendto(c, "", kLoopback, 5000) == ok(0));
    CHECK(n.recvfrom(a, 64) == ok(0));
    CHECK(all_bytes(b + n.addr2, 16, 0));

    // The receiver's SO_RCVBUF (9216 by default) holds what isn't read yet; more is dropped.
    std::memset(b + n.out, 'x', 0x10000);
    n.put_addr(n.addr, kLoopback, 5000);
    for (int i = 0; i < 3; i++) CHECK(n.call("sendto", c, n.out, 4000, 0, n.addr, 16) == ok(4000));
    CHECK(n.recv(a, 8192) == ok(4000));
    CHECK(n.recv(a, 8192) == ok(4000));
    CHECK(n.recv(a, 8192) == fail(SYS_NET_EWOULDBLOCK));
    CHECK(n.call("sendto", c, n.out, 65507, 0, n.addr, 16) == ok(65507)); // the most UDP carries (then dropped)
    CHECK(n.call("sendto", c, n.out, 65508, 0, n.addr, 16) == fail(SYS_NET_EMSGSIZE));
    REQUIRE(n.setopt(c, kSolSocket, kSoSndbuf, 100) == ok());
    CHECK(n.call("sendto", c, n.out, 101, 0, n.addr, 16) == fail(SYS_NET_EMSGSIZE)); // SO_SNDBUF bounds it too

    // A connected datagram socket: send without an address, sendto refused, only its peer's datagrams in.
    const s32 d = n.socket(kDgram);
    CHECK(n.connect(d, kLoopback, 5000) == ok());
    CHECK(n.connect(d, kLoopback, 5000) == fail(SYS_NET_EISCONN));
    const auto [dport, daddr] = n.name_of(d);
    CHECK(dport >= 49152);
    CHECK(daddr == kLoopback); // the route's source address
    CHECK(n.send(d, "via connect") == ok(11));
    CHECK(n.recv(a, 64) == ok(11));
    CHECK(n.received(11) == "via connect");
    CHECK(n.sendto(d, "x", kLoopback, 5000) == fail(SYS_NET_EISCONN));
    CHECK(n.sendto(a, "reply", kLoopback, dport) == ok(5));
    CHECK(n.sendto(c, "stranger", kLoopback, dport) == ok(8));
    REQUIRE(n.setopt(d, kSolSocket, kSoNbio, 1) == ok());
    CHECK(n.recv(d, 64) == ok(5));
    CHECK(n.received(5) == "reply");
    CHECK(n.recv(d, 64) == fail(SYS_NET_EWOULDBLOCK));
    // shutdown(SHUT_RD): reads give 0, later datagrams are dropped; SHUT_WR: sending fails with EPIPE.
    CHECK(n.sendto(a, "queued", kLoopback, dport) == ok(6));
    CHECK(n.call("shutdown", d, 0) == ok());
    CHECK(n.recv(d, 64) == ok(0));
    CHECK(n.call("shutdown", d, 1) == ok());
    CHECK(n.send(d, "x") == fail(SYS_NET_EPIPE));

    // An exact address wins over INADDR_ANY.
    const s32 wild = n.socket(kDgram), exact = n.socket(kDgram);
    REQUIRE(n.setopt(wild, kSolSocket, kSoReuseaddr, 1) == ok());
    REQUIRE(n.setopt(exact, kSolSocket, kSoReuseaddr, 1) == ok());
    REQUIRE(n.bind(wild, 0, 5100) == ok());
    REQUIRE(n.bind(exact, kLoopback, 5100) == ok());
    REQUIRE(n.setopt(exact, kSolSocket, kSoNbio, 1) == ok());
    CHECK(n.sendto(c, "exact", kLoopback, 5100) == ok(5));
    CHECK(n.recv(exact, 64) == ok(5));

    // P2P datagrams go by virtual port; the source carries the sender's.
    const s32 p = n.socket(kDgramP2p), q = n.socket(kDgramP2p);
    REQUIRE(n.bind(p, 0, 3658, 7) == ok());
    REQUIRE(n.bind(q, 0, 3658, 8) == ok());
    REQUIRE(n.setopt(q, kSolSocket, kSoNbio, 1) == ok());
    CHECK(n.sendto(p, "p2p", kLoopback, 3658, 0, 9) == ok(3)); // nobody on virtual port 9
    CHECK(n.recvfrom(q, 64) == fail(SYS_NET_EWOULDBLOCK));
    CHECK(n.sendto(p, "p2p", kLoopback, 3658, 0, 8) == ok(3));
    CHECK(n.recvfrom(q, 64) == ok(3));
    CHECK(n.received(3) == "p2p");
    CHECK(ps_read16(b, n.addr2 + 2) == 3658);
    CHECK(ps_read32(b, n.addr2 + 4) == kLoopback);
    CHECK(ps_read16(b, n.addr2 + 8) == 7);
}

TEST_CASE("sys_net loopback TCP: connect, accept, data both ways, EOF, EPIPE and flow control", "[hle][net]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread g(f, 0, "net");
    Net n(g, 0);
    const s32 l = n.socket(kStream);
    REQUIRE(n.bind(l, kLoopback, 7100) == ok());
    REQUIRE(n.call("listen", l, 1) == ok());

    const s32 c = n.socket(kStream);
    CHECK(n.connect(c, kLoopback, 7100) == ok());
    CHECK(n.connect(c, kLoopback, 7100) == fail(SYS_NET_EISCONN));
    CHECK(n.connect(n.socket(kStream), kLoopback, 7100) == fail(SYS_NET_ECONNREFUSED)); // the backlog (1) is full
    CHECK(n.connect(l, kLoopback, 7100) == fail(SYS_NET_EOPNOTSUPP));
    CHECK(n.call("listen", c, 1) == fail(SYS_NET_EINVAL)); // connected
    const auto [cport, caddr] = n.name_of(c);
    CHECK(cport >= 49152);
    CHECK(caddr == kLoopback);

    std::memset(b + n.addr2, 0xAA, 16);
    ps_write32(b, n.len, 32);
    const Outcome acc = n.call("accept", l, n.addr2, n.len);
    REQUIRE(acc.value > c);
    const s32 s = acc.value;
    CHECK(ps_read8(b, n.addr2) == 16);
    CHECK(ps_read8(b, n.addr2 + 1) == kAfInet);
    CHECK(ps_read16(b, n.addr2 + 2) == cport);
    CHECK(ps_read32(b, n.addr2 + 4) == kLoopback);
    CHECK(ps_read32(b, n.len) == 16);
    CHECK(n.name_of(s) == std::pair<u16, u32>{7100, kLoopback});

    // Bytes both ways; recvfrom on a stream gives a zero port and address (RPCS3).
    CHECK(n.send(c, "request") == ok(7));
    CHECK(n.recv(s, 3) == ok(3));
    CHECK(n.received(3) == "req");
    CHECK(n.recvfrom(s, 64) == ok(4));
    CHECK(n.received(4) == "uest");
    CHECK(ps_read8(b, n.addr2) == 16);
    CHECK(ps_read8(b, n.addr2 + 1) == kAfInet);
    CHECK(all_bytes(b + n.addr2 + 2, 14, 0));
    CHECK(n.send(s, "response") == ok(8));
    CHECK(n.recv(c, 64, kMsgPeek) == ok(8));
    CHECK(n.recv(c, 64) == ok(8));
    CHECK(n.received(8) == "response");
    CHECK(n.recv(c, 64, kMsgDontwait) == fail(SYS_NET_EWOULDBLOCK));
    CHECK(n.send(c, "abc") == ok(3));
    CHECK(n.send(c, "def") == ok(3));
    CHECK(n.recv(s, 6, kMsgWaitall) == ok(6));
    CHECK(n.received(6) == "abcdef");

    // SHUT_WR: the peer reads what is left, then EOF (0, again and again); the other direction still works.
    CHECK(n.send(c, "bye") == ok(3));
    CHECK(n.call("shutdown", c, 1) == ok());
    CHECK(n.send(c, "more") == fail(SYS_NET_EPIPE));
    CHECK(n.recv(s, 64) == ok(3));
    CHECK(n.recv(s, 64) == ok(0));
    CHECK(n.recv(s, 64) == ok(0));
    CHECK(n.send(s, "last") == ok(4));
    CHECK(n.recv(c, 64) == ok(4));
    // A close: the peer's sends fail with EPIPE.
    CHECK(n.call("socketclose", c) == ok());
    CHECK(n.send(s, "x") == fail(SYS_NET_EPIPE));
    CHECK(n.call("socketclose", s) == ok());

    // Flow control: SO_RCVBUF + SO_SNDBUF in flight (the accepted socket has the listener's options). A non-blocking
    // writer stops there.
    REQUIRE(n.setopt(l, kSolSocket, kSoRcvbuf, 50) == ok());
    const s32 c2 = n.socket(kStream);
    REQUIRE(n.setopt(c2, kSolSocket, kSoSndbuf, 100) == ok());
    REQUIRE(n.setopt(c2, kSolSocket, kSoNbio, 1) == ok());
    REQUIRE(n.connect(c2, kLoopback, 7100) == ok());
    const s32 s2 = n.call("accept", l, 0, 0).value;
    REQUIRE(s2 > 0);
    std::memset(b + n.out, 'y', 400);
    CHECK(n.call("send", c2, n.out, 200, 0) == ok(150));
    CHECK(n.call("send", c2, n.out, 10, 0) == fail(SYS_NET_EWOULDBLOCK));
    CHECK(n.recv(s2, 100) == ok(100));
    CHECK(n.call("send", c2, n.out, 200, 0) == ok(100));

    // A blocking writer waits for room and finishes when the reader drains the connection.
    REQUIRE(n.setopt(c2, kSolSocket, kSoNbio, 0) == ok());
    std::atomic<lv2::PpuThread*> parked{nullptr};
    Outcome wrote;
    std::thread writer([&] {
        GuestThread w(f, 1, "writer");
        Net wn(w, 1);
        std::memset(wn.b + wn.out, 'z', 400);
        parked = w.t;
        wrote = wn.call("send", c2, wn.out, 400, 0);
        parked = nullptr;
    });
    REQUIRE(wait_parked(parked));
    usize drained = 0;
    while (drained < 150 + 400) {
        const Outcome r = n.recv(s2, 64);
        REQUIRE(r.value > 0);
        drained += static_cast<usize>(r.value);
    }
    writer.join();
    CHECK(wrote == ok(400));
    CHECK(drained == 550);
    // SO_SNDTIMEO ends a blocked send with what was sent.
    REQUIRE(n.set_timeout(c2, kSoSndtimeo, 0, 20000) == ok());
    CHECK(n.call("send", c2, n.out, 200, 0) == ok(150));
    CHECK(n.call("send", c2, n.out, 200, 0) == fail(SYS_NET_EWOULDBLOCK));
    // SHUT_RD on the reader: what is queued goes and later bytes are dropped (the writer still succeeds).
    CHECK(n.call("shutdown", s2, 0) == ok());
    CHECK(n.recv(s2, 64) == ok(0));
    CHECK(n.call("send", c2, n.out, 200, 0) == ok(200));
}

TEST_CASE("sys_net socketselect: RPCS3's checks and counts, waits, timeouts and EINTR", "[hle][net]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread g(f, 0, "net");
    Net n(g, 0);
    const s32 u = n.socket(kDgram), t = n.socket(kStream); // 24, 25
    REQUIRE(n.bind(u, kLoopback, 7200) == ok());

    // 0 < nfds <= 1024; a set bit below nfds needs a socket.
    n.clear_sets();
    CHECK(n.select(0, std::pair<s64, s64>{0, 0}) == fail(SYS_NET_EINVAL));
    CHECK(n.select(1025, std::pair<s64, s64>{0, 0}) == fail(SYS_NET_EINVAL));
    n.add(n.rset(), 3);
    CHECK(n.select(26, std::pair<s64, s64>{0, 0}) == fail(SYS_NET_EBADF));
    CHECK(n.select(3, std::pair<s64, s64>{0, 0}) == ok(0)); // bit 3 isn't below nfds
    n.clear_sets();
    n.add(n.eset(), 3);
    CHECK(n.select(26, std::pair<s64, s64>{0, 0}) == ok(0)); // exceptfds alone selects nothing
    CHECK(n.empty(n.eset()));
    CHECK(n.call("socketselect", 26, kUnmapped, 0, 0, 0) == fail(SYS_NET_EFAULT));

    // A poll (timeout 0): the datagram socket is writable, the unconnected stream socket nothing.
    n.clear_sets();
    for (const s32 fd : {u, t}) {
        n.add(n.rset(), fd);
        n.add(n.wset(), fd);
    }
    std::memset(b + n.eset(), 0xFF, 128);
    CHECK(n.select(26, std::pair<s64, s64>{0, 0}) == ok(1));
    CHECK(n.empty(n.rset()));
    CHECK(n.has(n.wset(), u));
    CHECK_FALSE(n.has(n.wset(), t));
    CHECK(n.empty(n.eset())); // never set (RPCS3)

    // Readable after a loopback datagram; readable and writable counts once (RPCS3 counts sockets).
    REQUIRE(n.sendto(t, "x", kLoopback, 7200) == fail(SYS_NET_ENOTCONN));
    REQUIRE(n.sendto(n.socket(kDgram), "ping", kLoopback, 7200) == ok(4));
    n.clear_sets();
    n.add(n.rset(), u);
    n.add(n.wset(), u);
    CHECK(n.select(25, std::pair<s64, s64>{0, 0}) == ok(1));
    CHECK(n.has(n.rset(), u));
    CHECK(n.has(n.wset(), u));
    REQUIRE(n.recv(u, 64) == ok(4));

    // A timeout passes without an event: 0, the sets cleared.
    n.clear_sets();
    n.add(n.rset(), u);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(n.select(25, std::pair<s64, s64>{0, 50000}) == ok(0));
    CHECK(std::chrono::steady_clock::now() - t0 >= 50ms);
    CHECK(n.empty(n.rset()));

    // Without a timeout it waits, here for a datagram another thread sends.
    std::atomic<lv2::PpuThread*> parked{nullptr};
    Outcome got;
    bool readable = false;
    std::thread selector([&] {
        GuestThread r(f, 1, "selector");
        Net rn(r, 1);
        rn.clear_sets();
        rn.add(rn.rset(), u);
        parked = r.t;
        got = rn.select(25, std::nullopt);
        readable = rn.has(rn.rset(), u);
        parked = nullptr;
    });
    REQUIRE(wait_parked(parked));
    REQUIRE(n.sendto(n.socket(kDgram), "pong", kLoopback, 7200) == ok(4));
    selector.join();
    CHECK(got == ok(1));
    CHECK(readable);

    // A selected socket closed during the wait ends it with EINTR (RPCS3 abort_socket).
    REQUIRE(n.recv(u, 64) == ok(4));
    std::thread selector2([&] {
        GuestThread r(f, 2, "selector 2");
        Net rn(r, 2);
        rn.clear_sets();
        rn.add(rn.rset(), u);
        parked = r.t;
        got = rn.select(25, std::pair<s64, s64>{10, 0});
        parked = nullptr;
    });
    REQUIRE(wait_parked(parked));
    REQUIRE(n.call("socketclose", u) == ok());
    selector2.join();
    CHECK(got == fail(SYS_NET_EINTR));
}

TEST_CASE("sys_net_get_sockinfo(_ex): RPCS3's infoctl answers", "[hle][net]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread g(f, 0, "net");
    Net n(g, 0);
    const s32 u = n.socket(kDgram), l = n.socket(kStream), c = n.socket(kStream); // 24, 25, 26
    REQUIRE(n.bind(l, kLoopback, 7300) == ok());
    REQUIRE(n.call("listen", l, 4) == ok());
    REQUIRE(n.connect(c, kLoopback, 7300) == ok());
    const s32 s = n.call("accept", l, 0, 0).value; // 27
    REQUIRE(s == 27);
    REQUIRE(n.send(c, "abc") == ok(3));
    const s32 p = n.socket(kDgramP2p); // 28
    REQUIRE(n.bind(p, 0, 3658, 9) == ok());
    const u16 cport = n.name_of(c).first;

    const auto word = [&](u32 at, u32 i) { return ps_read32(b, at + 4 * i); };
    // s == -1: the count without a buffer, the first n in descriptor order with one.
    CHECK(n.call("sys_net_get_sockinfo", -1, 0, 0) == ok(5));
    CHECK(n.call("sys_net_get_sockinfo", -1, n.info, -1) == fail(SYS_NET_EINVAL));
    std::memset(b + n.info, 0xAA, 0x200);
    CHECK(n.call("sys_net_get_sockinfo", -1, n.info, 2) == ok(2));
    // sys_net_sockinfo_t: s, proto, recv_queue_length, send_queue_length, local_adr, local_port, remote_adr,
    // remote_port, state.
    const std::array<u32, 9> unbound_udp = {24, 17, 0, 0, 0, 0, 0, 0, 3}; // listed: not TCP, so OPENED
    const std::array<u32, 9> listening = {25, 6, 0, 0, kLoopback, 7300, 0, 0, 4}; // LISTEN
    for (u32 i = 0; i < 9; i++) {
        INFO("word " << i);
        CHECK(word(n.info, i) == unbound_udp[i]);
        CHECK(word(n.info + 36, i) == listening[i]);
    }
    CHECK(ps_read32(b, n.info + 72) == 0xAAAAAAAA);
    CHECK(n.call("sys_net_get_sockinfo", -1, n.info, 10) == ok(5));
    CHECK(n.call("sys_net_get_sockinfo", -1, n.info, 0) == ok(0));

    // One socket: a buffer and n >= 1, then the socket; the answer is 0.
    CHECK(n.call("sys_net_get_sockinfo", c, 0, 1) == fail(SYS_NET_EINVAL));
    CHECK(n.call("sys_net_get_sockinfo", c, n.info, 0) == fail(SYS_NET_EINVAL));
    CHECK(n.call("sys_net_get_sockinfo", 99, n.info, 1) == fail(SYS_NET_EBADF));
    CHECK(n.call("sys_net_get_sockinfo", c, kUnmapped, 1) == fail(SYS_NET_EFAULT));
    CHECK(n.call("sys_net_get_sockinfo", c, n.info, 1) == ok(0));
    const std::array<u32, 9> client = {26, 6, 0, 0, kLoopback, cport, kLoopback, 7300, 7}; // ESTABLISHED
    for (u32 i = 0; i < 9; i++) CHECK(word(n.info, i) == client[i]);
    CHECK(n.call("sys_net_get_sockinfo", s, n.info, 1) == ok(0));
    const std::array<u32, 9> server = {27, 6, 3, 0, kLoopback, 7300, kLoopback, cport, 7}; // 3 bytes queued
    for (u32 i = 0; i < 9; i++) CHECK(word(n.info, i) == server[i]);
    REQUIRE(n.bind(u, 0, 6672) == ok());
    CHECK(n.call("sys_net_get_sockinfo", u, n.info, 1) == ok(0));
    CHECK(word(n.info, 5) == 6672);
    CHECK(word(n.info, 8) == 3); // OPENED

    // The ex form (80 bytes): the socket type, the virtual ports and 8 reserved words follow.
    std::memset(b + n.info, 0xAA, 0x200);
    CHECK(n.call("sys_net_get_sockinfo_ex", p, n.info, 1, 0) == ok(0));
    const std::array<u32, 12> p2p = {28, 17, 0, 0, 0, 3658, 0, 0, 3, 6, 9, 0};
    for (u32 i = 0; i < 12; i++) {
        INFO("word " << i);
        CHECK(word(n.info, i) == p2p[i]);
    }
    CHECK(all_bytes(b + n.info + 48, 32, 0));
    CHECK(ps_read32(b, n.info + 80) == 0xAAAAAAAA);
    CHECK(n.call("sys_net_get_sockinfo_ex", -1, n.info, 5, 0) == ok(5));
    CHECK(word(n.info + 80 * 4, 0) == 28);
}

TEST_CASE("sys_net inet_addr, inet_pton, inet_ntop: BSD's conversions", "[hle][net]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread g(f, 0, "net");
    Net n(g, 0);

    const auto addr = [&](std::string_view s) {
        n.put_text(s);
        return static_cast<u32>(g.call("inet_addr", n.text));
    };
    CHECK(addr("1.2.3.4") == 0x01020304u);
    CHECK(addr("127.0.0.1") == kLoopback);
    CHECK(addr("0.0.0.0") == 0u);
    CHECK(addr("255.255.255.255") == 0xFFFFFFFFu); // INADDR_NONE's value, as in BSD
    CHECK(addr("0x7f.1") == kLoopback);           // a.b: b fills 24 bits
    CHECK(addr("010.0.0.1") == 0x08000001u);      // octal
    CHECK(addr("0X0A.0.0.1") == 0x0A000001u);
    CHECK(addr("1.2.3") == 0x01020003u);          // a.b.c: c fills 16 bits
    CHECK(addr("1.65535") == 0x0100FFFFu);
    CHECK(addr("3232235777") == 0xC0A80101u);     // one 32-bit number
    CHECK(addr("0") == 0u);
    CHECK(addr("1.2.3.4 and more") == 0x01020304u); // white space ends it
    CHECK(addr("1.2.3.4\n") == 0x01020304u);
    CHECK(addr("0x") == 0u); // libnet's parser takes a bare "0x" as 0 (BSD's refuses it)
    for (const std::string_view bad : {"", " 1.2.3.4", "1.2.3.4x", "1.2.3.256", "256.1.1.1", "1.2.3.4.5", "08.1.1.1",
             "1..2.3", "1.2.3.", ".1.2.3", "1.16777216", "1.2.65536", "a.b.c.d", "4294967296x"}) {
        INFO("\"" << bad << "\"");
        CHECK(addr(bad) == 0xFFFFFFFFu);
    }
    CHECK(g.call("inet_addr", kUnmapped) == 0xFFFFFFFFu); // RPCS3 faults

    const auto pton = [&](s32 af, std::string_view s) {
        n.put_text(s);
        std::memset(b + n.out, 0xAA, 16);
        return n.call("inet_pton", af, n.text, n.out);
    };
    const auto out_bytes = [&](usize count) { return std::vector<u8>(b + n.out, b + n.out + count); };
    CHECK(pton(kAfInet, "192.168.1.20") == ok(1));
    CHECK(out_bytes(5) == std::vector<u8>{192, 168, 1, 20, 0xAA});
    CHECK(pton(kAfInet, "0.0.0.0") == ok(1));
    CHECK(out_bytes(4) == std::vector<u8>{0, 0, 0, 0});
    // Decimal dotted quads only: no leading zeros, no shorthand, nothing after it.
    CHECK(pton(kAfInet, "1.2.3.4 ") == ok(1)); // libnet's inet_pton is inet_aton's parser: white space ends it
    for (const std::string_view bad : {"01.2.3.4", "1.2.3", "1.2.3.4.", "256.0.0.1", "0x1.2.3.4", "",
             "1..2.3", "1.2.3.4.5", "-1.2.3.4"}) {
        INFO("\"" << bad << "\"");
        CHECK(pton(kAfInet, bad) == ok(0));
        CHECK(all_bytes(b + n.out, 16, 0xAA)); // dst untouched
    }
    CHECK(pton(99, "1.2.3.4") == fail(SYS_NET_EAFNOSUPPORT));
    CHECK(n.call("inet_pton", kAfInet, kUnmapped, n.out) == fail(SYS_NET_EFAULT));
    n.put_text("1.2.3.4");
    CHECK(n.call("inet_pton", kAfInet, n.text, kUnmapped) == fail(SYS_NET_EFAULT));
    // IPv6 (AF_INET6 is 24 on the PS3).
    const auto v6 = [](std::initializer_list<u16> words) {
        std::vector<u8> out;
        for (const u16 w : words) {
            out.push_back(static_cast<u8>(w >> 8));
            out.push_back(static_cast<u8>(w));
        }
        return out;
    };
    CHECK(pton(kAfInet6, "::1") == ok(1));
    CHECK(out_bytes(16) == v6({0, 0, 0, 0, 0, 0, 0, 1}));
    CHECK(pton(kAfInet6, "::") == ok(1));
    CHECK(out_bytes(16) == v6({0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(pton(kAfInet6, "1:2:3:4:5:6:7:8") == ok(1));
    CHECK(out_bytes(16) == v6({1, 2, 3, 4, 5, 6, 7, 8}));
    CHECK(pton(kAfInet6, "FE80::1:aBcD") == ok(1));
    CHECK(out_bytes(16) == v6({0xfe80, 0, 0, 0, 0, 0, 1, 0xabcd}));
    CHECK(pton(kAfInet6, "1::") == ok(1));
    CHECK(out_bytes(16) == v6({1, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(pton(kAfInet6, "::ffff:1.2.3.4") == ok(1));
    CHECK(out_bytes(16) == v6({0, 0, 0, 0, 0, 0xffff, 0x0102, 0x0304}));
    for (const std::string_view bad : {"1:::2", "12345::", "1:2:3:4:5:6:7:8:9", ":1", "1:", "::1.2.3", "1::2::3",
             "1:2:3:4:5:6:7:8::", "::ffff:1.2.3.4.5", "g::1", "1.2.3.4"}) {
        INFO("\"" << bad << "\"");
        CHECK(pton(kAfInet6, bad) == ok(0));
    }

    const auto ntop = [&](s32 af, const std::vector<u8>& bytes, u32 size) {
        std::memcpy(b + n.out, bytes.data(), bytes.size());
        std::memset(b + n.text, 0xAA, 64);
        const u32 r = static_cast<u32>(g.call("inet_ntop", af, n.out, n.text, size));
        return std::pair<u32, std::string>{r, r ? std::string(reinterpret_cast<const char*>(b + n.text)) : ""};
    };
    CHECK(ntop(kAfInet, {1, 2, 3, 4}, 16) == std::pair<u32, std::string>{n.text, "1.2.3.4"});
    CHECK(ntop(kAfInet, {255, 255, 255, 255}, 16) == std::pair<u32, std::string>{n.text, "255.255.255.255"});
    CHECK(ntop(kAfInet, {1, 2, 3, 4}, 8) == std::pair<u32, std::string>{n.text, "1.2.3.4"}); // exactly enough
    CHECK(ntop(kAfInet, {1, 2, 3, 4}, 7).first == 0);
    CHECK(n.err() == SYS_NET_ENOSPC);
    CHECK(ps_read8(b, n.text) == 0xAA);
    CHECK(ntop(99, {1, 2, 3, 4}, 16).first == 0);
    CHECK(n.err() == SYS_NET_EAFNOSUPPORT);
    CHECK(g.call("inet_ntop", kAfInet, kUnmapped, n.text, 16) == 0);
    CHECK(n.err() == SYS_NET_EFAULT);
    // IPv6: lowercase groups without leading zeros, the longest run of zero groups (the first of equals) as "::",
    // a single zero group kept, IPv4-mapped and -compatible addresses with a dotted quad.
    const std::pair<std::vector<u8>, std::string_view> texts[] = {
        {v6({0, 0, 0, 0, 0, 0, 0, 1}), "::1"},
        {v6({0, 0, 0, 0, 0, 0, 0, 0}), "::"},
        {v6({1, 0, 0, 0, 0, 0, 0, 0}), "1::"},
        {v6({0x2001, 0xdb8, 0, 0, 0, 0xff00, 0x42, 0x8329}), "2001:db8::ff00:42:8329"},
        {v6({1, 0, 0, 2, 0, 0, 0, 3}), "1:0:0:2::3"},
        {v6({1, 0, 0, 0, 2, 0, 0, 0}), "1::2:0:0:0"},
        {v6({1, 0, 2, 3, 4, 5, 6, 7}), "1:0:2:3:4:5:6:7"},
        {v6({0xFE80, 0, 0, 0, 0, 0, 1, 0xABCD}), "fe80::1:abcd"},
        {v6({0, 0, 0, 0, 0, 0xffff, 0x0102, 0x0304}), "::ffff:1.2.3.4"},
        {v6({0, 0, 0, 0, 0, 0, 0x0102, 0x0304}), "::1.2.3.4"},
    };
    for (const auto& [bytes, text] : texts) {
        INFO(text);
        CHECK(ntop(kAfInet6, bytes, 64).second == text);
        CHECK(pton(kAfInet6, text) == ok(1)); // and back
        CHECK(out_bytes(16) == bytes);
    }
    CHECK(ntop(kAfInet6, v6({0, 0, 0, 0, 0, 0, 0, 1}), 3).first == 0);
    CHECK(n.err() == SYS_NET_ENOSPC);
}

TEST_CASE("sys_net gethostbyname without a name server: TRY_AGAIN, libnet's hosts, a numeric name", "[hle][net]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread g(f, 0, "net");
    Net n(g, 0);
    const u32 h_errno_loc = static_cast<u32>(g.call("_sys_net_h_errno_loc"));
    const auto h_errno_value = [&] { return static_cast<s32>(ps_read32(b, h_errno_loc)); };

    n.put_text("prod.ros.rockstargames.com");
    CHECK(g.call("gethostbyname", n.text) == 0);
    CHECK(h_errno_value() == 2); // TRY_AGAIN
    n.put_text("localhost"); // libnet's /etc/hosts: 127.0.0.1 localhost localhost.
    const u32 lh = static_cast<u32>(g.call("gethostbyname", n.text));
    REQUIRE(lh != 0);
    CHECK(h_errno_value() == 0);
    CHECK(std::string(reinterpret_cast<const char*>(b + ps_read32(b, lh))) == "localhost");
    CHECK(std::string(reinterpret_cast<const char*>(b + ps_read32(b, ps_read32(b, lh + 4)))) == "localhost.");
    CHECK(ps_read32(b, ps_read32(b, ps_read32(b, lh + 16))) == kLoopback);
    n.put_text("1.2.3.4."); // ends in a dot: a name
    CHECK(g.call("gethostbyname", n.text) == 0);
    CHECK(h_errno_value() == 2);
    n.put_text("1.2.3"); // digits and dots that inet_pton refuses
    CHECK(g.call("gethostbyname", n.text) == 0);
    CHECK(h_errno_value() == 1); // HOST_NOT_FOUND

    // A dotted quad: a hostent {h_name, h_aliases, h_addrtype, h_length, h_addr_list} without a lookup.
    n.put_text("127.0.0.1");
    const u32 h = static_cast<u32>(g.call("gethostbyname", n.text));
    REQUIRE(h != 0);
    const u32 name = ps_read32(b, h), aliases = ps_read32(b, h + 4), list = ps_read32(b, h + 16);
    CHECK(std::string(reinterpret_cast<const char*>(b + name)) == "127.0.0.1");
    CHECK(ps_read32(b, aliases) == 0);
    CHECK(ps_read32(b, h + 8) == static_cast<u32>(kAfInet));
    CHECK(ps_read32(b, h + 12) == 4);
    CHECK(ps_read32(b, ps_read32(b, list)) == kLoopback);
    CHECK(ps_read32(b, list + 4) == 0);

    // An unreadable name: NETDB_INTERNAL, errno EFAULT (RPCS3 faults).
    CHECK(g.call("gethostbyname", kUnmapped) == 0);
    CHECK(h_errno_value() == -1);
    CHECK(n.err() == SYS_NET_EFAULT);
}

// D-569: gethostbyaddr as NetBSD's gethnamaddr.c with libnet's hosts file, no name server.
TEST_CASE("sys_net gethostbyaddr without a name server: the hosts file's localhost, TRY_AGAIN, the checks",
    "[hle][net]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread g(f, 0, "net");
    Net n(g, 0);
    const u32 h_errno_loc = static_cast<u32>(g.call("_sys_net_h_errno_loc"));
    const auto h_errno_value = [&] { return static_cast<s32>(ps_read32(b, h_errno_loc)); };
    const u32 a = n.text;
    const auto put = [&](std::initializer_list<u8> bytes) {
        u32 i = 0;
        for (const u8 v : bytes) ps_write8(b, a + i++, v);
    };

    // 127.0.0.1 from the hosts file: localhost, alias "localhost.".
    put({127, 0, 0, 1});
    const u32 h = static_cast<u32>(g.call("gethostbyaddr", a, 4, kAfInet));
    REQUIRE(h != 0);
    CHECK(h_errno_value() == 0);
    CHECK(std::string(reinterpret_cast<const char*>(b + ps_read32(b, h))) == "localhost");
    CHECK(std::string(reinterpret_cast<const char*>(b + ps_read32(b, ps_read32(b, h + 4)))) == "localhost.");
    CHECK(ps_read32(b, h + 8) == static_cast<u32>(kAfInet));
    CHECK(ps_read32(b, h + 12) == 4);
    CHECK(ps_read32(b, ps_read32(b, ps_read32(b, h + 16))) == kLoopback);
    // ::ffff:127.0.0.1 is looked up as 127.0.0.1.
    put({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 127, 0, 0, 1});
    const u32 m = static_cast<u32>(g.call("gethostbyaddr", a, 16, kAfInet6));
    REQUIRE(m != 0);
    CHECK(ps_read32(b, m + 8) == static_cast<u32>(kAfInet));
    // Anything else needs a name server: none offline.
    put({8, 8, 8, 8});
    CHECK(g.call("gethostbyaddr", a, 4, kAfInet) == 0);
    CHECK(h_errno_value() == 2); // TRY_AGAIN
    put({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}); // ::1, not in the hosts file
    CHECK(g.call("gethostbyaddr", a, 16, kAfInet6) == 0);
    CHECK(h_errno_value() == 2);
    // IPv6 link-local and site-local addresses: HOST_NOT_FOUND without a lookup.
    put({0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
    CHECK(g.call("gethostbyaddr", a, 16, kAfInet6) == 0);
    CHECK(h_errno_value() == 1);
    put({0xFE, 0xC0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
    CHECK(g.call("gethostbyaddr", a, 16, kAfInet6) == 0);
    CHECK(h_errno_value() == 1);
    // A family other than INET and INET6, or a length not the family's: NETDB_INTERNAL with errno.
    put({127, 0, 0, 1});
    CHECK(g.call("gethostbyaddr", a, 4, 1) == 0);
    CHECK(h_errno_value() == -1);
    CHECK(n.err() == SYS_NET_EAFNOSUPPORT);
    CHECK(g.call("gethostbyaddr", a, 16, kAfInet) == 0);
    CHECK(h_errno_value() == -1);
    CHECK(n.err() == SYS_NET_EINVAL);
    CHECK(g.call("gethostbyaddr", kUnmapped, 4, kAfInet) == 0);
    CHECK(h_errno_value() == -1);
    CHECK(n.err() == SYS_NET_EFAULT);
}

TEST_CASE("sys_net setup: the pool's checks, a second initialisation", "[hle][net]")
{
    Fixture f;
    u8* b = f.base();
    REQUIRE(f.hle->link_internal_functions({"sys_net"}));
    GuestThread g(f, 0, "net");
    const u32 param = kData + 0x1FF000;
    const auto init = [&](u32 memory, u32 size, u32 flags) {
        ps_write32(b, param, memory);
        ps_write32(b, param + 4, size);
        ps_write32(b, param + 8, flags);
        return g.call("sys_net_initialize_network_ex", param);
    };
    const u64 einval = r3_of_code(0x80010216);
    CHECK(g.call("sys_net_initialize_network_ex", 0) == einval);
    CHECK(init(0, 0x10000, 0) == einval);
    CHECK(init(kNetPool, 0x1FFF, 0) == einval);
    CHECK(init(kNetPool, 0x10000, 2) == einval);
    CHECK(init(kNetPool, 0x10000, 0) == 0);
    CHECK(init(kNetPool, 0x10000, 0) == 0);
    CHECK(init(kNetPool, 0x10000, 1) == r3_of_code(0x80010210)); // flag 1: EBUSY when already initialised
    CHECK(g.call("sys_net_set_resolver_configurations", 1, 2, 3) == r3_of_code(0x80010201));
    CHECK(g.call("sys_net_set_resolver_configurations", 1, 2, 0) == 0);
    CHECK(g.call("sys_net_finalize_network") == 0);
    CHECK(g.call("sys_net_finalize_network") == 0);
}

TEST_CASE("cellSsl: init and end, no firmware certificates", "[hle][net]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread g(f, 0, "ssl");
    constexpr u32 kBuf = kData + 0x1000, kRequired = kData + 0x100;
    const u64 kNotInit = r3_of_code(0x80740001), kAlready = r3_of_code(0x80740002), kNoBuffer = r3_of_code(0x80740004);

    // libssl makes a heap over the pool it is given, so a null pool or size is CELL_SSL_ERROR_NO_BUFFER (D-530;
    // compared with Sony's module in test_ssl_compare.cpp). The whole set-up runs without firmware.
    CHECK(g.call("cellSslEnd") == kNotInit);
    CHECK(g.call("cellSslInit", 0, 0x10000) == kNoBuffer);
    CHECK(g.call("cellSslInit", kData, 0) == kNoBuffer);
    CHECK(g.call("cellSslInit", kData, 0x10000) == 0);
    CHECK(g.call("cellSslInit", 0, 0) == kAlready);
    CHECK(g.call("cellSslEnd") == 0);
    CHECK(g.call("cellSslEnd") == kNotInit);

    // Every certificate is missing (no firmware), as in RPCS3 without one: a size of 0, a buffer of zeros (all but the
    // last byte). No initialisation needed.
    constexpr u64 kAll = 0x03FFFFFFFFFFFFFFull;
    ps_write32(b, kRequired, 0xAAAAAAAA);
    std::memset(b + kBuf, 0xAA, 0x100);
    CHECK(g.call("cellSslCertificateLoader", kAll, kBuf, 64, kRequired) == 0);
    CHECK(ps_read32(b, kRequired) == 0);
    CHECK(all_bytes(b + kBuf, 0x100, 0xAA)); // with `required` the buffer isn't touched
    CHECK(g.call("cellSslCertificateLoader", kAll, kBuf, 64, 0) == 0);
    CHECK(all_bytes(b + kBuf, 63, 0));
    CHECK(all_bytes(b + kBuf + 63, 0x100 - 63, 0xAA));
    std::memset(b + kBuf, 0xAA, 0x100);
    CHECK(g.call("cellSslCertificateLoader", kAll, kBuf, 1, 0) == 0);
    CHECK(g.call("cellSslCertificateLoader", kAll, kBuf, 0, 0) == 0); // RPCS3 would clear 4 GB
    CHECK(all_bytes(b + kBuf, 0x100, 0xAA));
    CHECK(g.call("cellSslCertificateLoader", kAll, 0, 0, 0) == 0);
    // Pointers that aren't guest memory (RPCS3 faults).
    CHECK(g.call("cellSslCertificateLoader", kAll, kBuf, 64, kUnmapped) == kNoBuffer);
    CHECK(g.call("cellSslCertificateLoader", kAll, 0, 64, 0) == kNoBuffer);
}
