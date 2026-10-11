// cellRudp, pspocket's own (D-607, D-630 to D-633): Init, End and the handlers; contexts over loopback UDP and UDP
// P2P sockets in one process, with the internal I/O thread and with the game's own I/O (ProcessEvents, NetReceived, a
// RAW socket's packets through the event handler); connecting (Listen and Accept, two Initiates meeting, Activate),
// messages and streams in order, loss and reordering made by the send filter on the socket path (rudp.hpp),
// retransmission, the windows, a connection's end and its events, polling, the statistics, every argument check and
// error code. Two processes cannot talk here: one HLE process runs at a time and lv2's loopback is a process's own
// (D-523), so a RAW socket's packets carried by the test stand for the network. Synthetic: no game data.

#include "hle_test_util.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pspocket/core/log.hpp"
#include "pspocket/lv2/object.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"
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
              INVALID_ARGUMENT = 0x80770004, INVALID_OPTION = 0x80770005, INVALID_MUXMODE = 0x80770006,
              MEMORY = 0x80770007, CONN_RESET = 0x80770009, CONN_REFUSED = 0x8077000a, CONN_TIMEOUT = 0x8077000b,
              VERSION_MISMATCH = 0x8077000c, TRANSPORT_MISMATCH = 0x8077000d, QUALITY_MISMATCH = 0x8077000e,
              THREAD_IN_USE = 0x80770010, NOT_ACCEPTABLE = 0x80770011, MSG_TOO_LARGE = 0x80770012,
              NOT_BOUND = 0x80770013, CANCELLED = 0x80770014, INVALID_VPORT = 0x80770015, WOULDBLOCK = 0x80770016,
              VPORT_IN_USE = 0x80770017, INVALID_SOCKET = 0x80770019, BUFFER_TOO_SMALL = 0x8077001a,
              MSG_MALFORMED = 0x8077001b, ALREADY_BOUND = 0x8077001d, ALREADY_EXISTS = 0x8077001e,
              INVALID_POLL_ID = 0x8077001f, TOO_MANY_CONTEXTS = 0x80770020, IN_PROGRESS = 0x80770021,
              NO_EVENT_HANDLER = 0x80770022, PAYLOAD_TOO_LARGE = 0x80770023, END_OF_DATA = 0x80770024,
              ALREADY_ESTABLISHED = 0x80770025, KEEP_ALIVE_FAILURE = 0x80770026;

// The options (RPCS3 cellRudp.h), states, events and flags (D-630).
constexpr s32 kMaxPayload = 1, kSndbuf = 2, kRcvbuf = 3, kNodelay = 4, kDelivery = 5, kOrder = 6, kNonblock = 7,
              kStream = 8, kConnectionTimeout = 9, kCloseWaitTimeout = 10, kAggregation = 11, kLastError = 14,
              kReadTimeout = 15, kWriteTimeout = 16, kFlushTimeout = 17, kKeepAliveInterval = 18,
              kKeepAliveTimeout = 19;
constexpr u32 kIdle = 0, kClosed = 1, kSynSent = 2, kEstablished = 4, kCloseWait = 5;
constexpr u32 kEvClosed = 1, kEvEstablished = 2, kEvError = 3, kEvWritable = 4, kEvReadable = 5, kEvFlushed = 6;
constexpr u16 kPollRead = 1, kPollWrite = 2, kPollFlush = 4, kPollError = 8;
constexpr u8 kDontwait = 0x01, kLatencyCritical = 0x08;
constexpr s32 kSockStream = 1, kSockDgram = 2, kSockDgramP2p = 6;
constexpr u32 kLoopback = 0x7F000001;
constexpr u64 k5s = 5'000'000;

// Guest memory.
constexpr u32 kCode = kData, kCtxHandler = kData + 0x1000, kRawHandler = kData + 0x1008, kMalloc = kData + 0x1010,
              kFree = kData + 0x1018, kAllocator = kData + 0x1020, kPoolState = kData + 0x1040,
              kRingA = kData + 0x2000, kRingB = kData + 0x3000, kRingC = kData + 0x4000, kReleased = kData + 0x5000,
              kArgs = kData + 0x6000, kPeer = kData + 0x6100, kAddr = kData + 0x6200, kStatus = kData + 0x6400,
              kPollEvents = kData + 0x6800, kInfo = kData + 0x6C00, kPool = kData + 0x8000, kOut = kData + 0x40000,
              kIn = kData + 0x60000, kRx = kData + 0x80000, kNotGuest = 0xF0000000u;

// The guest code: the context handler, the event handler and an allocator.
void write_code(Fixture& f)
{
    u8* b = f.base();
    std::vector<u32> c;
    const auto load = [&](u32 reg, u32 value) {
        c.push_back(as::lis(reg, static_cast<s32>(value >> 16)));
        c.push_back(as::ori(reg, reg, value & 0xFFFF));
    };
    const auto here = [&] { return kCode + 4 * static_cast<u32>(c.size()); };
    // The context handler (ctx_id, event_id, error_code, ring): the record {ctx, event, error} at ring + 16 + 12 * (n %
    // 256), then the count n + 1 at ring + 0.
    const u32 ctx_handler = here();
    c.push_back(as::lwz(7, 0, 6));
    c.push_back(as::andi_(8, 7, 255));
    c.push_back(as::mulli(8, 8, 12));
    c.push_back(as::add(8, 8, 6));
    c.push_back(as::stw(3, 16, 8));
    c.push_back(as::stw(4, 20, 8));
    c.push_back(as::stw(5, 24, 8));
    c.push_back(as::lwsync());
    c.push_back(as::addi(7, 7, 1));
    c.push_back(as::stw(7, 0, 6));
    c.push_back(as::li(3, 0));
    c.push_back(as::blr());
    // The event handler (event, soc, data, len, addr, addrlen, arg): SEND goes out with lv2's sendto (710), its answer
    // the handler's; SOCKET_RELEASED is counted at arg + 0, the socket kept at arg + 4.
    const u32 raw_handler = here();
    c.push_back(as::cmpwi(0, 3, 1));
    const usize to_released = c.size();
    c.push_back(0);
    c.push_back(as::mr(3, 4));
    c.push_back(as::mr(4, 5));
    c.push_back(as::mr(5, 6));
    c.push_back(as::li(6, 0));
    c.push_back(as::li(11, 710));
    c.push_back(as::sc());
    c.push_back(as::blr());
    const usize released = c.size();
    c.push_back(as::lwz(10, 0, 9));
    c.push_back(as::addi(10, 10, 1));
    c.push_back(as::stw(4, 4, 9));
    c.push_back(as::lwsync());
    c.push_back(as::stw(10, 0, 9));
    c.push_back(as::li(3, 0));
    c.push_back(as::blr());
    c[to_released] = as::bc_rel(4, 2, static_cast<s32>(4 * (released - to_released))); // bne
    // app_malloc(size) over the pool state {next, mallocs, frees, last freed}: NULL when next is 0, else next, which
    // moves on by size.
    const u32 malloc_fn = here();
    load(4, kPoolState);
    c.push_back(as::lwz(5, 0, 4));
    c.push_back(as::cmpwi(0, 5, 0));
    const usize to_fail = c.size();
    c.push_back(0);
    c.push_back(as::add(6, 5, 3));
    c.push_back(as::stw(6, 0, 4));
    c.push_back(as::lwz(7, 4, 4));
    c.push_back(as::addi(7, 7, 1));
    c.push_back(as::stw(7, 4, 4));
    c.push_back(as::mr(3, 5));
    c.push_back(as::blr());
    const usize fail = c.size();
    c.push_back(as::li(3, 0));
    c.push_back(as::blr());
    c[to_fail] = as::bc_rel(12, 2, static_cast<s32>(4 * (fail - to_fail))); // beq
    // app_free(ptr): counted, the pointer kept.
    const u32 free_fn = here();
    load(4, kPoolState);
    c.push_back(as::lwz(7, 8, 4));
    c.push_back(as::addi(7, 7, 1));
    c.push_back(as::stw(7, 8, 4));
    c.push_back(as::stw(3, 12, 4));
    c.push_back(as::blr());
    REQUIRE(4 * c.size() <= kCtxHandler - kCode);
    for (u32 i = 0; i < c.size(); i++) ps_write32(b, kCode + 4 * i, c[i]);
    for (const auto& [opd, entry] : {std::pair{kCtxHandler, ctx_handler}, std::pair{kRawHandler, raw_handler},
             std::pair{kMalloc, malloc_fn}, std::pair{kFree, free_fn}}) {
        ps_write32(b, opd, entry);
        ps_write32(b, opd + 4, 0);
    }
    ps_write32(b, kAllocator, kMalloc);
    ps_write32(b, kAllocator + 4, kFree);
    for (const u32 ring : {kRingA, kRingB, kRingC, kReleased}) ps_write32(b, ring, 0);
    ps_write32(b, kPoolState, kPool);
    for (u32 i = 4; i < 16; i += 4) ps_write32(b, kPoolState + i, 0);
}

// A context handler that calls the library back: cellRudpProcessEvents(0), its answer at arg + 0xE00 and a count of
// its calls at arg + 0xE04.
constexpr u32 kNestedCode = kCode + 0x800, kNestedHandler = kData + 0x1030;
void write_nested_handler(Fixture& f)
{
    u8* b = f.base();
    const u32 process = f.hle->link_function("cellRudp", recomp::nid_of("cellRudpProcessEvents"),
        "cellRudpProcessEvents");
    REQUIRE(process);
    std::vector<u32> c;
    const auto load = [&](u32 reg, u32 value) {
        c.push_back(as::lis(reg, static_cast<s32>(value >> 16)));
        c.push_back(as::ori(reg, reg, value & 0xFFFF));
    };
    c.push_back(as::mflr(0));
    c.push_back(as::std_(0, 16, 1));
    c.push_back(as::stdu(1, -0x80, 1));
    c.push_back(as::std_(31, 0x70, 1));
    c.push_back(as::mr(31, 6));
    c.push_back(as::li(3, 0));
    load(12, process);
    c.push_back(as::lwz(0, 0, 12));
    c.push_back(as::lwz(2, 4, 12));
    c.push_back(as::mtctr(0));
    c.push_back(as::bctrl());
    c.push_back(as::stw(3, 0xE00, 31));
    c.push_back(as::lwz(4, 0xE04, 31));
    c.push_back(as::addi(4, 4, 1));
    c.push_back(as::lwsync());
    c.push_back(as::stw(4, 0xE04, 31));
    c.push_back(as::ld(31, 0x70, 1));
    c.push_back(as::addi(1, 1, 0x80));
    c.push_back(as::ld(0, 16, 1));
    c.push_back(as::mtlr(0));
    c.push_back(as::li(3, 0));
    c.push_back(as::blr());
    REQUIRE(4 * c.size() <= 0x400);
    for (u32 i = 0; i < c.size(); i++) ps_write32(b, kNestedCode + 4 * i, c[i]);
    ps_write32(b, kNestedHandler, kNestedCode);
    ps_write32(b, kNestedHandler + 4, 0);
}

// A process with the guest code, a TLS block for each thread the library makes, guest code interpreted.
struct RudpFixture : Fixture {
    RudpFixture()
    {
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
        ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
        write_code(*this);
    }
};

struct Rec {
    u32 ctx = 0, event = 0, error = 0;
};

// A ring the context handler wrote (another thread's: its count read with acquire order, the records before it).
std::vector<Rec> events(u8* b, u32 ring)
{
    const u32 n = load32_acquire(b, ring);
    std::vector<Rec> out;
    for (u32 i = n > 256 ? n - 256 : 0; i < n; i++) {
        const u32 at = ring + 16 + 12 * (i % 256);
        out.push_back({ps_read32(b, at), ps_read32(b, at + 4), ps_read32(b, at + 8)});
    }
    return out;
}

bool has_event_now(u8* b, u32 ring, s32 ctx, u32 event, std::optional<u32> err)
{
    for (const Rec& r : events(b, ring))
        if (r.ctx == static_cast<u32>(ctx) && r.event == event && (!err || r.error == *err)) return true;
    return false;
}

template <class F>
bool wait_for(F&& done, std::chrono::milliseconds limit = 5000ms)
{
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (done()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return done();
}

// Whether the handler has heard the event, waiting for it a while: the thread that makes an event calls the handler
// after it has changed the context's state, so a test that sees the state may be just before the call.
bool has_event(u8* b, u32 ring, s32 ctx, u32 event, std::optional<u32> err = std::nullopt)
{
    return wait_for([&] { return has_event_now(b, ring, ctx, event, err); });
}

// lv2's call `number` on the test's thread.
s32 sc(GuestThread& g, u32 number, std::initializer_list<u64> args)
{
    u32 i = 3;
    for (const u64 a : args) g.ctx.gpr[i++] = a;
    g.ctx.gpr[11] = number;
    g.f.kernel->syscall(g.ctx);
    return static_cast<s32>(g.ctx.gpr[3]);
}

void sockaddr(u8* b, u32 at, u16 port, u16 vport = 0, u32 addr = kLoopback)
{
    std::memset(b + at, 0, 16);
    ps_write8(b, at, 16);
    ps_write8(b, at + 1, 2);
    ps_write16(b, at + 2, port);
    ps_write32(b, at + 4, addr);
    ps_write16(b, at + 8, vport);
}

// A UDP socket bound to 127.0.0.1:port, or a UDP P2P one at SCE_NP_PORT and the virtual port `vport`.
s32 socket_at(GuestThread& g, u16 port, u16 p2p_vport = 0)
{
    const s32 s = sc(g, 713, {2, static_cast<u64>(p2p_vport ? kSockDgramP2p : kSockDgram), 0});
    REQUIRE(s >= 0);
    if (p2p_vport) sockaddr(g.f.base(), kAddr, 3658, p2p_vport, 0);
    else sockaddr(g.f.base(), kAddr, port);
    REQUIRE(sc(g, 701, {static_cast<u64>(s), kAddr, 16}) == 0);
    return s;
}

u64 code(u32 c) { return r3_of_code(c); }

s32 create(GuestThread& g, u32 ring)
{
    REQUIRE(g.call("cellRudpCreateContext", kCtxHandler, ring, kArgs) == 0);
    return static_cast<s32>(ps_read32(g.f.base(), kArgs));
}

u64 set_opt(GuestThread& g, s32 ctx, s32 option, u64 value, u32 len = 4)
{
    u8* b = g.f.base();
    switch (len) {
    case 1: ps_write8(b, kArgs + 0x40, static_cast<u8>(value)); break;
    case 2: ps_write16(b, kArgs + 0x40, static_cast<u16>(value)); break;
    case 4: ps_write32(b, kArgs + 0x40, static_cast<u32>(value)); break;
    default: ps_write64(b, kArgs + 0x40, value); break;
    }
    return g.call("cellRudpSetOption", ctx, option, kArgs + 0x40, len);
}

// An option's value in 8 bytes (all ones when the call fails).
u64 get_opt(GuestThread& g, s32 ctx, s32 option)
{
    if (g.call("cellRudpGetOption", ctx, option, kArgs + 0x48, 8) != 0) return ~u64{0};
    return ps_read64(g.f.base(), kArgs + 0x48);
}

u32 state_of(GuestThread& g, s32 ctx)
{
    if (g.call("cellRudpGetContextStatus", ctx, kStatus, 48) != 0) return ~0u;
    return ps_read32(g.f.base(), kStatus);
}

// A context bound to `soc` at `vport`, with timeouts that end a stuck test.
s32 bound(GuestThread& g, u32 ring, s32 soc, u16 vport, u8 mux = 0)
{
    const s32 c = create(g, ring);
    REQUIRE(g.call("cellRudpBind", c, soc, vport, mux) == 0);
    REQUIRE(set_opt(g, c, kReadTimeout, k5s, 8) == 0);
    REQUIRE(set_opt(g, c, kWriteTimeout, k5s, 8) == 0);
    REQUIRE(set_opt(g, c, kFlushTimeout, k5s, 8) == 0);
    return c;
}

std::vector<u8> message(u32 i, u32 size)
{
    std::vector<u8> m(size);
    for (u32 k = 0; k < size; k++) m[k] = static_cast<u8>(i * 31 + k * 7 + (k >> 8));
    return m;
}

u64 put(GuestThread& g, s32 ctx, const std::vector<u8>& data, u8 flags = 0)
{
    std::memcpy(g.f.base() + kOut, data.data(), data.size());
    return g.call("cellRudpWrite", ctx, kOut, data.size(), flags);
}

// A Read of up to `len` bytes: its answer, and what it read.
std::pair<u64, std::vector<u8>> take(GuestThread& g, s32 ctx, u32 len, u8 flags = 0, u32 info = 0)
{
    const u64 r = g.call("cellRudpRead", ctx, kIn, len, flags, info);
    const s32 n = static_cast<s32>(r);
    std::vector<u8> got;
    if (n > 0) got.assign(g.f.base() + kIn, g.f.base() + kIn + n);
    return {r, got};
}

// The library's statistics (D-630's CellRudpStatus): counter i.
u64 stat(GuestThread& g, u32 i)
{
    REQUIRE(g.call("cellRudpGetStatus", kStatus, 400) == 0);
    return ps_read64(g.f.base(), kStatus + 8 * i);
}
constexpr u32 kStatSentSyn = 10, kStatLost = 18, kStatRetransmitted = 19, kStatReordered = 20, kStatCurrent = 21,
              kStatAllocs = 38, kStatFrees = 39, kStatMemCurrent = 40, kStatEstablished = 42, kStatFailed = 43,
              kStatFailedRefused = 45;

// The send filter's verdicts, by packet: a test's rule, thread-safe.
struct Filter {
    std::mutex m;
    std::function<bool(const rudp::Header&)> drop;     // lost
    std::function<bool(const rudp::Header&)> hold;     // held back until the next packet has gone
    std::vector<rudp::Packet> held;
    u32 dropped = 0, reordered = 0;

    void install()
    {
        rudp::set_send_filter([this](rudp::Packet p, std::vector<rudp::Packet>& send) {
            std::lock_guard lock(m);
            const std::optional<rudp::Header> h = rudp::decode(p.bytes.data(), p.bytes.size());
            if (h && drop && drop(*h)) {
                dropped++;
                return;
            }
            if (h && hold && hold(*h) && held.empty()) {
                held.push_back(std::move(p));
                return;
            }
            send.push_back(std::move(p));
            if (!held.empty()) reordered++;
            for (rudp::Packet& x : held) send.push_back(std::move(x));
            held.clear();
        });
    }
};

// The log lines on `channel` from `level` up, kept while it lives; the other channels' lines from Info up still go to
// stderr.
struct LogLines {
    std::string channel;
    log::Level old = log::min_level();
    std::mutex m;
    std::vector<std::string> lines;

    LogLines(std::string_view chan, log::Level level) : channel(chan)
    {
        log::set_min_level(std::min(level, old));
        log::set_sink(
            [](void* user, log::Level l, std::string_view c, std::string_view message) {
                auto* self = static_cast<LogLines*>(user);
                if (c == self->channel) {
                    std::lock_guard lock(self->m);
                    self->lines.emplace_back(message);
                } else if (l >= log::Level::Info) {
                    const std::string_view name = log::level_name(l);
                    std::fprintf(stderr, "[%.*s] %.*s: %.*s\n", static_cast<int>(name.size()), name.data(),
                        static_cast<int>(c.size()), c.data(), static_cast<int>(message.size()), message.data());
                }
            },
            this);
    }
    ~LogLines()
    {
        log::set_sink(nullptr, nullptr);
        log::set_min_level(old);
    }
    LogLines(const LogLines&) = delete;
    LogLines& operator=(const LogLines&) = delete;

    // How many contain `text`.
    usize count(std::string_view text)
    {
        std::lock_guard lock(m);
        return static_cast<usize>(std::ranges::count_if(
            lines, [&](const std::string& s) { return s.find(text) != std::string::npos; }));
    }
};

} // namespace

TEST_CASE("cellRudp: Init, End and the event handler as RPCS3 has them", "[hle][rudp]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    constexpr u32 kAlloc = kData + 0x100;

    // Nothing before Init.
    CHECK(g.call("cellRudpEnd") == code(NOT_INITIALIZED));
    CHECK(g.call("cellRudpSetEventHandler", 0x10200, kData) == code(NOT_INITIALIZED));

    // The library's own allocator; one Init only.
    CHECK(g.call("cellRudpInit", 0) == 0);
    CHECK(g.call("cellRudpInit", 0) == code(ALREADY_INITIALIZED));
    CHECK(g.call("cellRudpSetEventHandler", 0x10200, kData) == 0);
    CHECK(g.call("cellRudpSetEventHandler", 0, 0) == 0); // no handler
    CHECK(g.call("cellRudpEnd") == 0);
    CHECK(g.call("cellRudpEnd") == code(NOT_INITIALIZED));
    CHECK(g.call("cellRudpSetEventHandler", 0x10200, kData) == code(NOT_INITIALIZED));

    // The game's allocator: two descriptors.
    ps_write32(b, kAlloc, 0x10300);
    ps_write32(b, kAlloc + 4, 0x10310);
    CHECK(g.call("cellRudpInit", kAlloc) == 0);
    CHECK(g.call("cellRudpInit", kAlloc) == code(ALREADY_INITIALIZED));
    CHECK(g.call("cellRudpInit", kNotGuest) == code(ALREADY_INITIALIZED)); // RPCS3 checks that first
    CHECK(g.call("cellRudpEnd") == 0);

    // An allocator that is not guest memory initialises nothing.
    CHECK(g.call("cellRudpInit", kNotGuest) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpEnd") == code(NOT_INITIALIZED));

    // The whole library pspocket's, firmware-first.
    CHECK(registry().is_firmware_first("cellRudp"));
    for (const char* name : {"cellRudpEnableInternalIOThread", "cellRudpSetMaxSegmentSize", "cellRudpGetMaxSegmentSize",
             "cellRudpCreateContext", "cellRudpSetOption", "cellRudpGetOption", "cellRudpGetContextStatus",
             "cellRudpGetStatus", "cellRudpGetLocalInfo", "cellRudpGetRemoteInfo", "cellRudpAccept", "cellRudpBind",
             "cellRudpListen", "cellRudpInitiate", "cellRudpActivate", "cellRudpTerminate", "cellRudpRead",
             "cellRudpWrite", "cellRudpGetSizeReadable", "cellRudpGetSizeWritable", "cellRudpFlush",
             "cellRudpPollCreate", "cellRudpPollDestroy", "cellRudpPollControl", "cellRudpPollWait",
             "cellRudpPollCancel", "cellRudpNetReceived", "cellRudpProcessEvents"}) {
        INFO(name);
        const Function* fn = GuestThread::lookup(name);
        REQUIRE(fn != nullptr);
        CHECK(fn->module == "cellRudp");
        CHECK(!fn->unverified); // the library as a whole is firmware-first (D-563)
    }
}

TEST_CASE("cellRudp: each process its own library", "[hle][rudp]")
{
    {
        Fixture f;
        GuestThread g(f, 0, "game");
        CHECK(g.call("cellRudpInit", 0) == 0);
    }
    Fixture f;
    GuestThread g(f, 0, "game");
    CHECK(g.call("cellRudpEnd") == code(NOT_INITIALIZED));
    CHECK(g.call("cellRudpInit", 0) == 0);
}

TEST_CASE("cellRudp: every call before Init, and the argument checks of the library and its contexts",
    "[hle][rudp]")
{
    RudpFixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();

    // Every function: NOT_INITIALIZED.
    const std::vector<std::pair<const char*, std::vector<u64>>> all = {
        {"cellRudpEnableInternalIOThread", {0x4000, 1000}}, {"cellRudpSetMaxSegmentSize", {1400}},
        {"cellRudpGetMaxSegmentSize", {kArgs}}, {"cellRudpCreateContext", {kCtxHandler, 0, kArgs}},
        {"cellRudpSetOption", {1, kStream, kArgs, 4}},
        {"cellRudpGetOption", {1, kStream, kArgs, 4}}, {"cellRudpGetContextStatus", {1, kStatus, 48}},
        {"cellRudpGetStatus", {kStatus, 400}}, {"cellRudpGetLocalInfo", {1, 0, 0, 0, 0, 0}},
        {"cellRudpGetRemoteInfo", {1, 0, 0, 0}}, {"cellRudpAccept", {1}}, {"cellRudpBind", {1, 24, 1, 0}},
        {"cellRudpListen", {1, 1}}, {"cellRudpInitiate", {1, kPeer, 16, 1}}, {"cellRudpActivate", {1, kPeer, 16}},
        {"cellRudpTerminate", {1}}, {"cellRudpRead", {1, kIn, 16, 0, 0}}, {"cellRudpWrite", {1, kOut, 16, 0}},
        {"cellRudpGetSizeReadable", {1}}, {"cellRudpGetSizeWritable", {1}}, {"cellRudpFlush", {1}},
        {"cellRudpPollCreate", {4}}, {"cellRudpPollDestroy", {1}}, {"cellRudpPollControl", {1, 1, 1, 1}},
        {"cellRudpPollWait", {1, kPollEvents, 4, 0}}, {"cellRudpPollCancel", {1}},
        {"cellRudpNetReceived", {24, kIn, 32, kPeer, 16}}, {"cellRudpProcessEvents", {0}}};
    for (const auto& [name, args] : all) {
        INFO(name);
        const Function& fn = GuestThread::find(name);
        for (usize i = 0; i < args.size(); i++) g.ctx.gpr[3 + i] = args[i];
        fn.fn(g.ctx, b);
        CHECK(g.ctx.gpr[3] == code(NOT_INITIALIZED));
    }

    REQUIRE(g.call("cellRudpInit", 0) == 0);

    // The maximum segment size: 1400 at first, 128 to 65507.
    REQUIRE(g.call("cellRudpGetMaxSegmentSize", kArgs) == 0);
    CHECK(ps_read16(b, kArgs) == 1400);
    CHECK(g.call("cellRudpGetMaxSegmentSize", 0) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpGetMaxSegmentSize", kNotGuest) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpSetMaxSegmentSize", 127) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpSetMaxSegmentSize", 65508) == code(INVALID_ARGUMENT));
    REQUIRE(g.call("cellRudpSetMaxSegmentSize", 1000) == 0);
    REQUIRE(g.call("cellRudpGetMaxSegmentSize", kArgs) == 0);
    CHECK(ps_read16(b, kArgs) == 1000);
    REQUIRE(g.call("cellRudpSetMaxSegmentSize", 1400) == 0);

    // Contexts: an ID's place must be guest memory; IDs from 1; unknown IDs refused everywhere.
    CHECK(g.call("cellRudpCreateContext", kCtxHandler, kRingA, 0) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpCreateContext", kCtxHandler, kRingA, kNotGuest) == code(INVALID_ARGUMENT));
    const s32 c = create(g, kRingA);
    CHECK(c >= 1);
    const s32 c2 = create(g, kRingA);
    CHECK(c2 == c + 1);
    for (const s32 bad : {0, -1, c2 + 1}) {
        INFO("context " << bad);
        CHECK(g.call("cellRudpSetOption", bad, kStream, kArgs, 4) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpGetOption", bad, kStream, kArgs, 4) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpGetContextStatus", bad, kStatus, 48) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpGetLocalInfo", bad, 0, 0, 0, 0, 0) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpGetRemoteInfo", bad, 0, 0, 0) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpAccept", bad) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpBind", bad, 24, 1, 0) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpListen", bad, 1) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpInitiate", bad, kPeer, 16, 1) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpActivate", bad, kPeer, 16) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpTerminate", bad) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpRead", bad, kIn, 16, 0, 0) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpWrite", bad, kOut, 16, 0) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpGetSizeReadable", bad) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpGetSizeWritable", bad) == code(INVALID_CONTEXT_ID));
        CHECK(g.call("cellRudpFlush", bad) == code(INVALID_CONTEXT_ID));
    }

    // Options: the defaults; 1, 2, 4 or 8 bytes; 12, 13 and past 19 are no option; LAST_ERROR only read.
    CHECK(get_opt(g, c, kMaxPayload) == 1400 - 32);
    CHECK(get_opt(g, c, kSndbuf) == 65536);
    CHECK(get_opt(g, c, kRcvbuf) == 65536);
    CHECK(get_opt(g, c, kNodelay) == 0);
    CHECK(get_opt(g, c, kDelivery) == 1);
    CHECK(get_opt(g, c, kOrder) == 1);
    CHECK(get_opt(g, c, kNonblock) == 0);
    CHECK(get_opt(g, c, kStream) == 0);
    CHECK(get_opt(g, c, kConnectionTimeout) == 30'000'000);
    CHECK(get_opt(g, c, kCloseWaitTimeout) == 1'000'000);
    CHECK(get_opt(g, c, kAggregation) == 10'000);
    CHECK(get_opt(g, c, kLastError) == 0);
    CHECK(get_opt(g, c, kReadTimeout) == 0);
    CHECK(get_opt(g, c, kWriteTimeout) == 0);
    CHECK(get_opt(g, c, kFlushTimeout) == 0);
    CHECK(get_opt(g, c, kKeepAliveInterval) == 1'000'000);
    CHECK(get_opt(g, c, kKeepAliveTimeout) == 10'000'000);
    for (const s32 bad : {0, 12, 13, 20, -1}) {
        INFO("option " << bad);
        CHECK(set_opt(g, c, bad, 1) == code(INVALID_OPTION));
        CHECK(g.call("cellRudpGetOption", c, bad, kArgs, 4) == code(INVALID_OPTION));
    }
    CHECK(set_opt(g, c, kLastError, 0) == code(INVALID_OPTION));
    CHECK(g.call("cellRudpSetOption", c, kStream, kArgs, 3) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpSetOption", c, kStream, 0, 4) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpSetOption", c, kStream, kNotGuest, 4) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpGetOption", c, kStream, kArgs, 0) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpGetOption", c, kStream, kNotGuest, 4) == code(INVALID_ARGUMENT));
    for (const u32 len : {1u, 2u, 4u, 8u}) {
        INFO("width " << len);
        REQUIRE(set_opt(g, c, kNodelay, 1, len) == 0);
        CHECK(get_opt(g, c, kNodelay) == 1);
        REQUIRE(set_opt(g, c, kNodelay, 0, len) == 0);
        CHECK(get_opt(g, c, kNodelay) == 0);
    }
    REQUIRE(g.call("cellRudpGetOption", c, kSndbuf, kArgs, 2) == code(BUFFER_TOO_SMALL)); // 65536 needs 3 bytes
    REQUIRE(g.call("cellRudpGetOption", c, kSndbuf, kArgs, 4) == 0);
    CHECK(ps_read32(b, kArgs) == 65536);
    REQUIRE(g.call("cellRudpGetOption", c, kDelivery, kArgs, 1) == 0);
    CHECK(ps_read8(b, kArgs) == 1);
    CHECK(set_opt(g, c, kMaxPayload, 0) == code(INVALID_ARGUMENT));
    CHECK(set_opt(g, c, kMaxPayload, 1369) == code(PAYLOAD_TOO_LARGE)); // more than a segment's payload
    CHECK(set_opt(g, c, kMaxPayload, 1368) == 0);
    CHECK(set_opt(g, c, kMaxPayload, 100) == 0);
    CHECK(get_opt(g, c, kMaxPayload) == 100);
    CHECK(set_opt(g, c, kSndbuf, 255) == code(INVALID_ARGUMENT));
    CHECK(set_opt(g, c, kRcvbuf, (16u << 20) + 1) == code(INVALID_ARGUMENT));
    CHECK(set_opt(g, c, kSndbuf, 4096) == 0);
    CHECK(get_opt(g, c, kSndbuf) == 4096);
    CHECK(set_opt(g, c, kKeepAliveTimeout, 123456789, 8) == 0);
    CHECK(get_opt(g, c, kKeepAliveTimeout) == 123456789);

    // A context's status: IDLE, no parent; at most the size asked for, zeros past 48 bytes.
    std::memset(b + kStatus, 0xEE, 64);
    REQUIRE(g.call("cellRudpGetContextStatus", c, kStatus, 64) == 0);
    CHECK(ps_read32(b, kStatus) == kIdle);
    CHECK(ps_read32(b, kStatus + 4) == 0);
    for (u32 i = 48; i < 64; i++) CHECK(ps_read8(b, kStatus + i) == 0);
    std::memset(b + kStatus, 0xEE, 64);
    REQUIRE(g.call("cellRudpGetContextStatus", c, kStatus, 2) == 0);
    CHECK(ps_read8(b, kStatus + 2) == 0xEE);
    CHECK(g.call("cellRudpGetContextStatus", c, kStatus, 0) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpGetContextStatus", c, kNotGuest, 48) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpGetStatus", kStatus, 0) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpGetStatus", kNotGuest, 400) == code(INVALID_ARGUMENT));
    CHECK(stat(g, kStatCurrent) == 2);
    CHECK(stat(g, kStatAllocs) == 2);

    // Not bound: no info, no connection, nothing to read or write.
    CHECK(g.call("cellRudpGetLocalInfo", c, kArgs, 0, 0, 0, 0) == code(NOT_BOUND));
    CHECK(g.call("cellRudpListen", c, 1) == code(NOT_BOUND));
    sockaddr(b, kPeer, 4000);
    CHECK(g.call("cellRudpInitiate", c, kPeer, 16, 1) == code(NOT_BOUND));
    CHECK(g.call("cellRudpActivate", c, kPeer, 16) == code(NOT_BOUND));
    CHECK(g.call("cellRudpRead", c, kIn, 16, 0, 0) == code(NOT_BOUND));
    CHECK(g.call("cellRudpWrite", c, kOut, 16, 0) == code(NOT_BOUND));
    CHECK(g.call("cellRudpFlush", c) == code(NOT_BOUND));
    CHECK(g.call("cellRudpAccept", c) == code(NOT_ACCEPTABLE));
    CHECK(g.call("cellRudpRead", c, kIn, 0, 0, 0) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpRead", c, kNotGuest, 16, 0, 0) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpRead", c, kIn, 16, 0, kNotGuest) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpWrite", c, kOut, 0, 0) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpWrite", c, 0, 16, 0) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpGetSizeReadable", c) == 0);
    CHECK(g.call("cellRudpGetSizeWritable", c) == 4096);
    // Remote info before a peer: zeros.
    std::memset(b + kAddr, 0xEE, 16);
    ps_write32(b, kArgs, 16);
    ps_write16(b, kArgs + 4, 0xEEEE);
    REQUIRE(g.call("cellRudpGetRemoteInfo", c, kAddr, kArgs, kArgs + 4) == 0);
    for (u32 i = 0; i < 16; i++) CHECK(ps_read8(b, kAddr + i) == 0);
    CHECK(ps_read32(b, kArgs) == 16);
    CHECK(ps_read16(b, kArgs + 4) == 0);
    CHECK(g.call("cellRudpGetRemoteInfo", c, kAddr, 0, 0) == code(INVALID_ARGUMENT)); // an address without its length

    // Bind: a UDP or UDP P2P socket only; mux modes 0 and 1; a vport each.
    const s32 udp = socket_at(g, 4000);
    const s32 tcp = sc(g, 713, {2, kSockStream, 0});
    REQUIRE(tcp >= 0);
    CHECK(g.call("cellRudpBind", c, 1000, 1, 0) == code(INVALID_SOCKET));
    CHECK(g.call("cellRudpBind", c, tcp, 1, 0) == code(INVALID_SOCKET));
    CHECK(g.call("cellRudpBind", c, udp, 1, 2) == code(INVALID_MUXMODE));
    CHECK(g.call("cellRudpBind", c, udp, 1, 1) == code(NO_EVENT_HANDLER));
    REQUIRE(g.call("cellRudpBind", c, udp, 7, 0) == 0);
    CHECK(g.call("cellRudpBind", c, udp, 8, 0) == code(ALREADY_BOUND));
    CHECK(g.call("cellRudpBind", c2, udp, 7, 0) == code(VPORT_IN_USE));
    REQUIRE(g.call("cellRudpSetEventHandler", kRawHandler, kReleased) == 0);
    CHECK(g.call("cellRudpBind", c2, udp, 8, 1) == code(INVALID_MUXMODE)); // the socket's contexts share its mode
    REQUIRE(g.call("cellRudpBind", c2, udp, 0, 0) == 0);                    // the lowest free vport
    REQUIRE(g.call("cellRudpGetLocalInfo", c2, kArgs, kAddr, kArgs + 4, kArgs + 8, kArgs + 10) == 0);
    CHECK(static_cast<s32>(ps_read32(b, kArgs)) == udp);
    CHECK(ps_read16(b, kArgs + 8) == 1);
    CHECK(ps_read8(b, kArgs + 10) == 0);
    REQUIRE(g.call("cellRudpSetEventHandler", 0, 0) == 0);
    // Local info: the socket's address (getsockname), each output on its own.
    ps_write32(b, kArgs + 4, 16);
    REQUIRE(g.call("cellRudpGetLocalInfo", c, kArgs, kAddr, kArgs + 4, kArgs + 8, kArgs + 10) == 0);
    CHECK(static_cast<s32>(ps_read32(b, kArgs)) == udp);
    CHECK(ps_read16(b, kAddr + 2) == 4000);
    CHECK(ps_read32(b, kAddr + 4) == kLoopback);
    CHECK(ps_read32(b, kArgs + 4) == 16);
    CHECK(ps_read16(b, kArgs + 8) == 7);
    REQUIRE(g.call("cellRudpGetLocalInfo", c, 0, 0, 0, kArgs + 8, 0) == 0);
    CHECK(g.call("cellRudpGetLocalInfo", c, kNotGuest, 0, 0, 0, 0) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpGetLocalInfo", c, 0, kAddr, 0, 0, 0) == code(INVALID_ARGUMENT));

    // Initiate's and Activate's peer: 16 bytes of guest memory; a vport to go to.
    CHECK(g.call("cellRudpInitiate", c, 0, 16, 1) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpInitiate", c, kPeer, 15, 1) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpInitiate", c, kNotGuest, 16, 1) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpInitiate", c, kPeer, 16, 0) == code(INVALID_VPORT));
    CHECK(g.call("cellRudpActivate", c, kPeer, 8) == code(INVALID_ARGUMENT));
    // Bound, not connected: NOT_BOUND for the data calls.
    CHECK(g.call("cellRudpRead", c, kIn, 16, 0, 0) == code(NOT_BOUND));
    CHECK(g.call("cellRudpWrite", c, kOut, 16, 0) == code(NOT_BOUND));
    // A listener: no connection of its own.
    REQUIRE(g.call("cellRudpListen", c2, 0) == 0);
    CHECK(g.call("cellRudpInitiate", c2, kPeer, 16, 1) == code(NOT_ACCEPTABLE));
    CHECK(g.call("cellRudpRead", c2, kIn, 16, 0, 0) == code(NOT_ACCEPTABLE));
    CHECK(g.call("cellRudpWrite", c2, kOut, 16, 0) == code(NOT_ACCEPTABLE));
    REQUIRE(set_opt(g, c2, kNonblock, 1) == 0);
    CHECK(g.call("cellRudpAccept", c2) == code(WOULDBLOCK));
    REQUIRE(set_opt(g, c2, kNonblock, 0) == 0);
    REQUIRE(set_opt(g, c2, kReadTimeout, 20'000, 8) == 0);
    CHECK(g.call("cellRudpAccept", c2) == code(WOULDBLOCK)); // its READ_TIMEOUT
    // A connection being made: what it agreed on can no longer change; a second start is IN_PROGRESS.
    REQUIRE(g.call("cellRudpInitiate", c, kPeer, 16, 99) == 0); // nobody there: SYN after SYN
    CHECK(state_of(g, c) == kSynSent);
    CHECK(g.call("cellRudpInitiate", c, kPeer, 16, 99) == code(IN_PROGRESS));
    CHECK(g.call("cellRudpActivate", c, kPeer, 16) == code(IN_PROGRESS));
    CHECK(g.call("cellRudpListen", c, 1) == code(IN_PROGRESS));
    CHECK(set_opt(g, c, kStream, 1) == code(IN_PROGRESS));
    CHECK(set_opt(g, c, kDelivery, 0) == code(IN_PROGRESS));
    CHECK(set_opt(g, c, kOrder, 0) == code(IN_PROGRESS));
    CHECK(set_opt(g, c, kNodelay, 1) == 0); // the others still do
    CHECK(stat(g, kStatSentSyn) >= 1);
    // Written before the handshake ends: kept for it (a message up to MAX_PAYLOAD).
    CHECK(put(g, c, message(0, 100)) == 100);
    CHECK(put(g, c, message(0, 101)) == code(MSG_TOO_LARGE));
    CHECK(g.call("cellRudpGetSizeWritable", c) == 4096 - 100);
    // Remote info: the peer as given, its vport.
    ps_write32(b, kArgs, 16);
    REQUIRE(g.call("cellRudpGetRemoteInfo", c, kAddr, kArgs, kArgs + 4) == 0);
    CHECK(std::memcmp(b + kAddr, b + kPeer, 16) == 0);
    CHECK(ps_read16(b, kArgs + 4) == 99);

    // Terminated: the IDs go; the records go back.
    REQUIRE(g.call("cellRudpTerminate", c) == 0);
    REQUIRE(g.call("cellRudpTerminate", c2) == 0);
    CHECK(g.call("cellRudpTerminate", c) == code(INVALID_CONTEXT_ID));
    CHECK(stat(g, kStatCurrent) == 0);
    CHECK(stat(g, kStatFrees) == 2);

    // Polls: 1 to 1024 contexts; unknown sets; the operations' checks.
    CHECK(g.call("cellRudpPollCreate", 0) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpPollCreate", 1025) == code(INVALID_ARGUMENT));
    const s32 poll = static_cast<s32>(g.call("cellRudpPollCreate", 1));
    CHECK(poll >= 1);
    const s32 d = create(g, kRingA), e = create(g, kRingA);
    CHECK(g.call("cellRudpPollControl", poll + 1, 1, d, kPollRead) == code(INVALID_POLL_ID));
    CHECK(g.call("cellRudpPollControl", poll, 1, 999, kPollRead) == code(INVALID_CONTEXT_ID));
    CHECK(g.call("cellRudpPollControl", poll, 4, d, kPollRead) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpPollControl", poll, 1, d, 0x10) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpPollControl", poll, 2, d, kPollRead) == code(INVALID_CONTEXT_ID));
    CHECK(g.call("cellRudpPollControl", poll, 3, d, 0) == code(INVALID_CONTEXT_ID));
    REQUIRE(g.call("cellRudpPollControl", poll, 1, d, kPollRead) == 0);
    CHECK(g.call("cellRudpPollControl", poll, 1, d, kPollRead) == code(ALREADY_EXISTS));
    CHECK(g.call("cellRudpPollControl", poll, 1, e, kPollRead) == code(MEMORY)); // the set holds one
    REQUIRE(g.call("cellRudpPollControl", poll, 2, d, kPollWrite) == 0);
    CHECK(g.call("cellRudpPollWait", poll + 1, kPollEvents, 4, 0) == code(INVALID_POLL_ID));
    CHECK(g.call("cellRudpPollWait", poll, kPollEvents, 0, 0) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpPollWait", poll, kNotGuest, 4, 0) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpPollWait", poll, kPollEvents, 4, 0) == 0); // an idle context: nothing
    CHECK(g.call("cellRudpPollWait", poll, kPollEvents, 4, 20'000) == 0);
    REQUIRE(g.call("cellRudpPollControl", poll, 3, d, 0) == 0);
    CHECK(g.call("cellRudpPollCancel", poll + 1) == code(INVALID_POLL_ID));
    REQUIRE(g.call("cellRudpPollCancel", poll) == 0);
    CHECK(g.call("cellRudpPollDestroy", poll + 1) == code(INVALID_POLL_ID));
    REQUIRE(g.call("cellRudpPollDestroy", poll) == 0);
    CHECK(g.call("cellRudpPollDestroy", poll) == code(INVALID_POLL_ID));

    // NetReceived: a datagram (1 to 65535 bytes) and its source address; a socket a context uses; an RUDP packet.
    const s32 other = socket_at(g, 4001);
    REQUIRE(g.call("cellRudpBind", d, other, 1, 0) == 0);
    sockaddr(b, kPeer, 4000);
    std::memset(b + kIn, 0, 64);
    CHECK(g.call("cellRudpNetReceived", other, kIn, 0, kPeer, 16) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpNetReceived", other, kIn, 64, kPeer, 15) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpNetReceived", other, kNotGuest, 64, kPeer, 16) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpNetReceived", other, kIn, 64, 0, 16) == code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpNetReceived", udp, kIn, 64, kPeer, 16) == code(INVALID_SOCKET)); // no context uses it
    CHECK(g.call("cellRudpNetReceived", other, kIn, 64, kPeer, 16) == code(MSG_MALFORMED)); // no magic
    CHECK(g.call("cellRudpNetReceived", other, kIn, 16, kPeer, 16) == code(MSG_MALFORMED)); // too short
    rudp::Header h;
    h.type = static_cast<rudp::Type>(9);
    rudp::encode(h, b + kIn);
    CHECK(g.call("cellRudpNetReceived", other, kIn, 32, kPeer, 16) == code(MSG_MALFORMED)); // no such type
    h.type = rudp::Type::Ack;
    h.dst = 1;
    rudp::encode(h, b + kIn);
    CHECK(g.call("cellRudpNetReceived", other, kIn, 32, kPeer, 16) == 0); // nobody's: answered with a RST

    // ProcessEvents and the I/O thread: either works; a second thread THREAD_IN_USE.
    CHECK(g.call("cellRudpProcessEvents", 0) == 0);
    CHECK(g.call("cellRudpProcessEvents", 5'000) == 0);
    REQUIRE(g.call("cellRudpEnableInternalIOThread", 0x4000, 1000) == 0);
    CHECK(g.call("cellRudpEnableInternalIOThread", 0x4000, 1000) == code(THREAD_IN_USE));
    CHECK(g.call("cellRudpProcessEvents", 0) == 0);
    REQUIRE(g.call("cellRudpEnd") == 0);
    CHECK(g.call("cellRudpCreateContext", kCtxHandler, kRingA, kArgs) == code(NOT_INITIALIZED));
}

TEST_CASE("cellRudp: the contexts' records from the game's allocator; MEMORY and TOO_MANY_CONTEXTS", "[hle][rudp]")
{
    RudpFixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    REQUIRE(g.call("cellRudpInit", kAllocator) == 0);
    const s32 c = create(g, kRingA);
    CHECK(load32_acquire(b, kPoolState + 4) == 1); // one app_malloc
    CHECK(load32_acquire(b, kPoolState) == kPool + 0x100);
    REQUIRE(g.call("cellRudpTerminate", c) == 0);
    CHECK(load32_acquire(b, kPoolState + 8) == 1); // one app_free, of that record
    CHECK(load32_acquire(b, kPoolState + 12) == kPool);
    ps_write32(b, kPoolState, 0); // the allocator has nothing left
    CHECK(g.call("cellRudpCreateContext", kCtxHandler, kRingA, kArgs) == code(MEMORY));
    REQUIRE(g.call("cellRudpEnd") == 0);

    // The library's own allocator: 256 contexts at most.
    REQUIRE(g.call("cellRudpInit", 0) == 0);
    std::vector<s32> made;
    for (u32 i = 0; i < 256; i++) made.push_back(create(g, kRingA));
    CHECK(g.call("cellRudpCreateContext", kCtxHandler, kRingA, kArgs) == code(TOO_MANY_CONTEXTS));
    REQUIRE(g.call("cellRudpTerminate", made.back()) == 0);
    CHECK(g.call("cellRudpCreateContext", kCtxHandler, kRingA, kArgs) == 0);
    CHECK(stat(g, kStatCurrent) == 256);
    CHECK(stat(g, kStatMemCurrent) == 256 * 0x100);
    REQUIRE(g.call("cellRudpEnd") == 0); // every record back
}

TEST_CASE("cellRudp: two contexts over loopback with the internal I/O thread: Listen, Accept, messages in order",
    "[hle][rudp]")
{
    RudpFixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    REQUIRE(g.call("cellRudpInit", 0) == 0);
    REQUIRE(g.call("cellRudpEnableInternalIOThread", 0x8000, 1000) == 0);
    const s32 sa = socket_at(g, 5000), sb = socket_at(g, 5001);
    const s32 server = bound(g, kRingA, sa, 10), client = bound(g, kRingB, sb, 20);
    REQUIRE(g.call("cellRudpListen", server, 4) == 0);
    sockaddr(b, kPeer, 5000);
    REQUIRE(g.call("cellRudpInitiate", client, kPeer, 16, 10) == 0);
    const s32 conn = static_cast<s32>(g.call("cellRudpAccept", server));
    REQUIRE(conn > client);
    REQUIRE(wait_for([&] { return state_of(g, client) == kEstablished; }));
    CHECK(state_of(g, conn) == kEstablished);
    CHECK(g.call("cellRudpInitiate", client, kPeer, 16, 10) == code(ALREADY_ESTABLISHED));
    CHECK(g.call("cellRudpActivate", conn, kPeer, 16) == code(ALREADY_ESTABLISHED));
    CHECK(g.call("cellRudpListen", client, 1) == code(ALREADY_ESTABLISHED));
    CHECK(set_opt(g, client, kStream, 1) == code(ALREADY_ESTABLISHED));
    CHECK(g.call("cellRudpBind", client, sb, 30, 0) == code(ALREADY_BOUND));
    CHECK(has_event(b, kRingB, client, kEvEstablished, 0));
    CHECK(has_event(b, kRingA, server, kEvReadable)); // the listener: one to accept
    // The accepted context: the listener's handler and argument; its parent the listener; the listener's children.
    REQUIRE(g.call("cellRudpGetContextStatus", conn, kStatus, 48) == 0);
    CHECK(ps_read32(b, kStatus + 4) == static_cast<u32>(server));
    REQUIRE(g.call("cellRudpGetContextStatus", server, kStatus, 48) == 0);
    CHECK(ps_read32(b, kStatus + 8) == 1);
    // Each side's view of the other.
    ps_write32(b, kArgs, 16);
    REQUIRE(g.call("cellRudpGetRemoteInfo", conn, kAddr, kArgs, kArgs + 4) == 0);
    CHECK(ps_read16(b, kAddr + 2) == 5001);
    CHECK(ps_read32(b, kAddr + 4) == kLoopback);
    CHECK(ps_read16(b, kArgs + 4) == 20);
    REQUIRE(g.call("cellRudpGetLocalInfo", conn, kArgs, 0, 0, kArgs + 8, 0) == 0);
    CHECK(static_cast<s32>(ps_read32(b, kArgs)) == sa);
    CHECK(ps_read16(b, kArgs + 8) == 10);

    // Messages both ways, in order, each whole; the read information.
    for (u32 i = 0; i < 200; i++) {
        INFO("message " << i);
        REQUIRE(put(g, client, message(i, 1 + (i * 37) % 600)) == 1 + (i * 37) % 600);
    }
    for (u32 i = 0; i < 200; i++) {
        INFO("message " << i);
        const auto [r, got] = take(g, conn, 2048, 0, kInfo);
        REQUIRE(r == 1 + (i * 37) % 600);
        CHECK(got == message(i, 1 + (i * 37) % 600));
        CHECK(ps_read8(b, kInfo) == 12);
    }
    REQUIRE(put(g, conn, message(7, 300), kLatencyCritical) == 300);
    {
        const auto [r, got] = take(g, client, 2048, 0, kInfo);
        REQUIRE(r == 300);
        CHECK(got == message(7, 300));
        CHECK(ps_read8(b, kInfo + 5) == kLatencyCritical);
    }
    // A buffer smaller than the message: BUFFER_TOO_SMALL, the message kept; nothing waiting: WOULDBLOCK.
    REQUIRE(put(g, client, message(9, 500)) == 500);
    REQUIRE(wait_for([&] { return g.call("cellRudpGetSizeReadable", conn) == 500; }));
    CHECK(take(g, conn, 499).first == code(BUFFER_TOO_SMALL));
    CHECK(take(g, conn, 500).second == message(9, 500));
    CHECK(take(g, conn, 500, kDontwait).first == code(WOULDBLOCK));
    REQUIRE(set_opt(g, conn, kReadTimeout, 30'000, 8) == 0);
    CHECK(take(g, conn, 500).first == code(WOULDBLOCK)); // the timeout
    // A blocked reader is woken by the data.
    std::atomic<lv2::PpuThread*> parked{nullptr};
    std::pair<u64, std::vector<u8>> got_late;
    REQUIRE(set_opt(g, conn, kReadTimeout, k5s, 8) == 0);
    std::thread reader([&] {
        GuestThread r(f, 1, "reader");
        parked = r.t;
        got_late.first = r.call("cellRudpRead", conn, kIn + 0x8000, 2048, 0, 0);
        const s32 n = static_cast<s32>(got_late.first);
        if (n > 0) got_late.second.assign(b + kIn + 0x8000, b + kIn + 0x8000 + n);
        parked = nullptr;
    });
    REQUIRE(wait_for([&] {
        lv2::PpuThread* p = parked.load();
        return p && p->waiter.waiting_on().kind == lv2::WaitKind::Net;
    }));
    REQUIRE(put(g, client, message(11, 64)) == 64);
    reader.join();
    CHECK(got_late.first == 64);
    CHECK(got_late.second == message(11, 64));

    // The statistics: two connections made, the contexts, what the client sent.
    CHECK(stat(g, kStatEstablished) == 2);
    CHECK(stat(g, kStatCurrent) == 3);
    REQUIRE(g.call("cellRudpGetContextStatus", client, kStatus, 48) == 0);
    CHECK(ps_read32(b, kStatus + 16) > 200); // packets sent
    CHECK(ps_read64(b, kStatus + 24) > 50'000);

    // The client ends: the accepted context reads what was left, then END_OF_DATA; CLOSED (no error) for it.
    REQUIRE(put(g, client, message(12, 100)) == 100);
    REQUIRE(g.call("cellRudpTerminate", client) == 0);
    REQUIRE(wait_for([&] { return state_of(g, conn) == kCloseWait; }));
    CHECK(has_event(b, kRingA, conn, kEvClosed, 0));
    CHECK(take(g, conn, 2048).second == message(12, 100));
    CHECK(take(g, conn, 2048).first == code(END_OF_DATA));
    CHECK(put(g, conn, message(1, 10)) == code(END_OF_DATA));
    REQUIRE(g.call("cellRudpTerminate", conn) == 0);
    REQUIRE(g.call("cellRudpTerminate", server) == 0);
    REQUIRE(wait_for([&] { return stat(g, kStatCurrent) == 0; })); // the client's lingering connection gone too
    REQUIRE(g.call("cellRudpEnd") == 0);
}

TEST_CASE("cellRudp: a stream: bytes in order, cut into segments, whatever the reads' sizes", "[hle][rudp]")
{
    RudpFixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    REQUIRE(g.call("cellRudpInit", 0) == 0);
    REQUIRE(g.call("cellRudpEnableInternalIOThread", 0x8000, 1000) == 0);
    REQUIRE(g.call("cellRudpSetMaxSegmentSize", 600) == 0);
    const s32 sa = socket_at(g, 5100), sb = socket_at(g, 5101);
    const s32 server = bound(g, kRingA, sa, 1), client = bound(g, kRingB, sb, 2);
    REQUIRE(set_opt(g, server, kStream, 1) == 0);
    REQUIRE(set_opt(g, client, kStream, 1) == 0);
    REQUIRE(g.call("cellRudpListen", server, 1) == 0);
    sockaddr(b, kPeer, 5100);
    REQUIRE(g.call("cellRudpInitiate", client, kPeer, 16, 1) == 0);
    const s32 conn = static_cast<s32>(g.call("cellRudpAccept", server));
    REQUIRE(conn > 0);
    CHECK(get_opt(g, conn, kStream) == 1);

    // About 100 KB: more than the reader's window, less than the window and the writer's buffer together.
    std::vector<u8> sent, got;
    for (u32 i = 0; i < 40; i++) {
        const std::vector<u8> m = message(i, 1 + (i * 997) % 5000);
        REQUIRE(put(g, client, m) == m.size());
        sent.insert(sent.end(), m.begin(), m.end());
    }
    u32 k = 0;
    while (got.size() < sent.size()) {
        const auto [r, part] = take(g, conn, 1 + (k++ * 211) % 3000);
        REQUIRE(static_cast<s32>(r) > 0);
        got.insert(got.end(), part.begin(), part.end());
    }
    CHECK(got == sent);
    REQUIRE(g.call("cellRudpFlush", client) == 0); // all acknowledged
    REQUIRE(g.call("cellRudpGetContextStatus", client, kStatus, 48) == 0);
    CHECK(ps_read32(b, kStatus + 16) >= sent.size() / (600 - 32)); // segments of the payload, at least
    // Write's flags: DONTWAIT writes what the buffer takes (more than its size when acknowledgements free room
    // meanwhile), not all of it while the reader's window is shut; then WOULDBLOCK.
    REQUIRE(set_opt(g, client, kSndbuf, 1000) == 0);
    REQUIRE(set_opt(g, conn, kRcvbuf, 256) == 0);
    const u64 n = put(g, client, message(1, 4000), kDontwait);
    CHECK(static_cast<s32>(n) > 0);
    CHECK(static_cast<s32>(n) < 4000);
    REQUIRE(wait_for([&] { // writing on until the reader's window is shut and the buffer stays full
        put(g, client, message(2, 4000), kDontwait);
        std::this_thread::sleep_for(10ms);
        return g.call("cellRudpGetSizeWritable", client) == 0;
    }));
    CHECK(put(g, client, message(3, 10), kDontwait) == code(WOULDBLOCK));
    REQUIRE(g.call("cellRudpEnd") == 0);
}

TEST_CASE("cellRudp: Initiate meets Initiate, Activate meets Initiate and Activate; UDP P2P sockets", "[hle][rudp]")
{
    RudpFixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    REQUIRE(g.call("cellRudpInit", 0) == 0);
    REQUIRE(g.call("cellRudpEnableInternalIOThread", 0x8000, 1000) == 0);
    const auto pair_up = [&](const char* how, bool p2p, u8 kind) {
        INFO(how);
        const s32 sa = p2p ? socket_at(g, 0, 40) : socket_at(g, 5200);
        const s32 sb = p2p ? socket_at(g, 0, 41) : socket_at(g, 5201);
        const s32 a = bound(g, kRingA, sa, 3), c = bound(g, kRingB, sb, kind == 2 ? 3 : 4);
        const auto peer = [&](u32 at, bool to_a) {
            if (p2p) sockaddr(b, at, 3658, to_a ? 40 : 41);
            else sockaddr(b, at, to_a ? 5200 : 5201);
        };
        peer(kPeer, false);
        peer(kPeer + 16, true);
        switch (kind) {
        case 0: // both initiate, each to the other's vport
            REQUIRE(g.call("cellRudpInitiate", a, kPeer, 16, 4) == 0);
            REQUIRE(g.call("cellRudpInitiate", c, kPeer + 16, 16, 3) == 0);
            break;
        case 1: // a waits for c, which initiates
            REQUIRE(g.call("cellRudpActivate", a, kPeer, 16) == 0);
            REQUIRE(g.call("cellRudpInitiate", c, kPeer + 16, 16, 3) == 0);
            break;
        case 3: // both initiate, c a while after a: a's first SYN finds c not started, dropped, not refused
        case 4: // c initiates a while before a activates
            if (kind == 3) REQUIRE(g.call("cellRudpInitiate", a, kPeer, 16, 4) == 0);
            else REQUIRE(g.call("cellRudpInitiate", c, kPeer + 16, 16, 3) == 0);
            std::this_thread::sleep_for(100ms);
            if (kind == 3) REQUIRE(g.call("cellRudpInitiate", c, kPeer + 16, 16, 3) == 0);
            else REQUIRE(g.call("cellRudpActivate", a, kPeer, 16) == 0);
            break;
        default: // both activate at the same vport
            REQUIRE(g.call("cellRudpActivate", a, kPeer, 16) == 0);
            REQUIRE(g.call("cellRudpActivate", c, kPeer + 16, 16) == 0);
            break;
        }
        REQUIRE(wait_for([&] { return state_of(g, a) == kEstablished && state_of(g, c) == kEstablished; }));
        CHECK(has_event(b, kRingA, a, kEvEstablished));
        CHECK(has_event(b, kRingB, c, kEvEstablished));
        REQUIRE(put(g, a, message(1, 99)) == 99);
        REQUIRE(put(g, c, message(2, 77)) == 77);
        CHECK(take(g, c, 128).second == message(1, 99));
        CHECK(take(g, a, 128).second == message(2, 77));
        ps_write32(b, kArgs, 16);
        REQUIRE(g.call("cellRudpGetRemoteInfo", a, kAddr, kArgs, kArgs + 4) == 0);
        CHECK(ps_read16(b, kArgs + 4) == (kind == 2 ? 3 : 4)); // learnt from the SYN when activated
        REQUIRE(g.call("cellRudpTerminate", a) == 0);
        REQUIRE(g.call("cellRudpTerminate", c) == 0);
        REQUIRE(wait_for([&] { return stat(g, kStatCurrent) == 0; }));
        sc(g, 714, {static_cast<u64>(sa)});
        sc(g, 714, {static_cast<u64>(sb)});
    };
    pair_up("two Initiates", false, 0);
    pair_up("Activate and Initiate", false, 1);
    pair_up("two Activates", false, 2);
    pair_up("two Initiates over UDP P2P", true, 0);
    pair_up("Activate and Initiate over UDP P2P", true, 1);
    pair_up("two Initiates, one later", false, 3);
    pair_up("an Initiate before the Activate", true, 4);
    REQUIRE(g.call("cellRudpEnd") == 0);
}

namespace {

// Message i of the loss test: its number in its first byte.
std::vector<u8> numbered(u32 i)
{
    std::vector<u8> m = message(i, 50 + (i * 13) % 900);
    m[0] = static_cast<u8>(i);
    return m;
}

} // namespace

TEST_CASE("cellRudp: lost and reordered packets: retransmission, every message once and in order", "[hle][rudp]")
{
    Filter filter; // first: the library (in the fixture) may call it until the process ends
    RudpFixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    REQUIRE(g.call("cellRudpInit", 0) == 0);
    REQUIRE(g.call("cellRudpEnableInternalIOThread", 0x8000, 1000) == 0);
    filter.install();
    const auto run = [&](const char* what, bool delivery, bool order, bool stream) {
        INFO(what);
        const s32 sa = socket_at(g, 5300), sb = socket_at(g, 5301);
        const s32 server = bound(g, kRingA, sa, 1), client = bound(g, kRingB, sb, 2);
        for (const s32 x : {server, client}) {
            REQUIRE(set_opt(g, x, kDelivery, delivery) == 0);
            REQUIRE(set_opt(g, x, kOrder, order) == 0);
            REQUIRE(set_opt(g, x, kStream, stream) == 0);
        }
        REQUIRE(g.call("cellRudpListen", server, 1) == 0);
        sockaddr(b, kPeer, 5300);
        REQUIRE(g.call("cellRudpInitiate", client, kPeer, 16, 1) == 0);
        const s32 conn = static_cast<s32>(g.call("cellRudpAccept", server));
        REQUIRE(conn > 0);
        const u64 retransmitted = stat(g, kStatRetransmitted), lost = stat(g, kStatLost);
        {
            std::lock_guard lock(filter.m);
            // Every third DATA segment lost the first time it goes; every fifth held back behind the next packet.
            filter.drop = [](const rudp::Header& h) {
                return h.type == rudp::Type::Data && !h.retries && h.seq % 3 == 0;
            };
            filter.hold = [](const rudp::Header& h) {
                return h.type == rudp::Type::Data && !h.retries && h.seq % 5 == 0;
            };
            filter.dropped = filter.reordered = 0;
        }
        constexpr u32 kCount = 120;
        std::vector<u8> sent;
        for (u32 i = 0; i < kCount; i++) {
            const std::vector<u8> m = numbered(i);
            REQUIRE(put(g, client, m) == m.size());
            sent.insert(sent.end(), m.begin(), m.end());
        }
        if (delivery) {
            if (stream) {
                std::vector<u8> got;
                while (got.size() < sent.size()) {
                    const auto [r, part] = take(g, conn, 4096);
                    REQUIRE(static_cast<s32>(r) > 0);
                    got.insert(got.end(), part.begin(), part.end());
                }
                CHECK(got == sent);
            } else {
                std::set<u32> seen;
                u32 last = 0;
                bool in_order = true;
                for (u32 i = 0; i < kCount; i++) {
                    const auto [r, got] = take(g, conn, 2048);
                    REQUIRE(static_cast<s32>(r) > 0);
                    const u32 index = got[0];
                    CHECK(got == numbered(index));
                    CHECK(!seen.contains(index));
                    in_order = in_order && (i == 0 || index > last);
                    last = index;
                    seen.insert(index);
                }
                CHECK(seen.size() == kCount);
                if (order) CHECK(in_order);
            }
            REQUIRE(g.call("cellRudpFlush", client) == 0);
            CHECK(stat(g, kStatRetransmitted) > retransmitted);
            CHECK(stat(g, kStatLost) > lost);
            REQUIRE(g.call("cellRudpGetContextStatus", client, kStatus, 48) == 0);
            CHECK(ps_read32(b, kStatus + 40) > 0); // its retransmissions
            CHECK(ps_read32(b, kStatus + 12) > 0); // and the losses it found
        } else {
            // Without delivery guarantees: what came, once each, whole; in increasing order when ordered (a message
            // held back behind a later one is dropped then); the rest lost, nothing sent again.
            REQUIRE(set_opt(g, conn, kReadTimeout, 300'000, 8) == 0);
            std::set<u32> seen;
            s32 last = -1;
            for (;;) {
                const auto [r, got] = take(g, conn, 2048);
                if (r == code(WOULDBLOCK)) break;
                REQUIRE(static_cast<s32>(r) > 0);
                const u32 index = got[0];
                CHECK(got == numbered(index));
                CHECK(!seen.contains(index));
                if (order) CHECK(static_cast<s32>(index) > last);
                last = static_cast<s32>(index);
                seen.insert(index);
            }
            CHECK(seen.size() < kCount);
            CHECK(seen.size() >= kCount / 3);
            CHECK(stat(g, kStatRetransmitted) == retransmitted);
        }
        {
            std::lock_guard lock(filter.m);
            CHECK(filter.dropped > 0);
            CHECK(filter.reordered > 0);
            filter.drop = nullptr;
            filter.hold = nullptr;
        }
        CHECK(stat(g, kStatReordered) > 0);
        for (const s32 x : {client, conn, server}) REQUIRE(g.call("cellRudpTerminate", x) == 0);
        REQUIRE(wait_for([&] { return stat(g, kStatCurrent) == 0; }));
        sc(g, 714, {static_cast<u64>(sa)});
        sc(g, 714, {static_cast<u64>(sb)});
    };
    run("reliable, ordered", true, true, false);
    run("reliable, unordered", true, false, false);
    run("a stream", true, true, true);
    run("unreliable, ordered", false, true, false);
    run("unreliable, unordered", false, false, false);
    rudp::set_send_filter(nullptr);
    REQUIRE(g.call("cellRudpEnd") == 0);
}

TEST_CASE("cellRudp: the windows: a reader that does not read stops the writer; WRITABLE, Flush and FLUSHED",
    "[hle][rudp]")
{
    RudpFixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    REQUIRE(g.call("cellRudpInit", 0) == 0);
    REQUIRE(g.call("cellRudpEnableInternalIOThread", 0x8000, 1000) == 0);
    const s32 sa = socket_at(g, 5400), sb = socket_at(g, 5401);
    const s32 server = bound(g, kRingA, sa, 1), client = bound(g, kRingB, sb, 2);
    REQUIRE(set_opt(g, server, kRcvbuf, 8192) == 0); // the accepted context's too
    REQUIRE(set_opt(g, client, kSndbuf, 6000) == 0);
    REQUIRE(set_opt(g, client, kNonblock, 1) == 0);
    REQUIRE(g.call("cellRudpListen", server, 1) == 0);
    sockaddr(b, kPeer, 5400);
    REQUIRE(g.call("cellRudpInitiate", client, kPeer, 16, 1) == 0);
    const s32 conn = static_cast<s32>(g.call("cellRudpAccept", server));
    REQUIRE(conn > 0);
    REQUIRE(wait_for([&] { return state_of(g, client) == kEstablished; }));

    // 1000-byte messages until the send buffer stays full: the reader takes about its window, the writer holds the
    // rest.
    u32 written = 0;
    REQUIRE(wait_for([&] {
        while (put(g, client, message(written, 1000)) == 1000) written++;
        std::this_thread::sleep_for(20ms); // what was in flight lands, its acknowledgements free the writer's room
        return g.call("cellRudpGetSizeWritable", client) < 1000;
    }));
    const u64 readable = g.call("cellRudpGetSizeReadable", conn);
    CHECK(readable <= 8192 + 2 * 1368); // its window, and a segment or two past it at most
    CHECK(readable >= 8192 - 1000);
    CHECK(g.call("cellRudpGetSizeWritable", client) < 1000);
    CHECK(put(g, client, message(0, 1000)) == code(WOULDBLOCK));
    CHECK(g.call("cellRudpFlush", client) == code(WOULDBLOCK)); // non-blocking: not all acknowledged yet
    // Polling the writer: FLUSH not ready, WRITE not ready.
    const s32 poll = static_cast<s32>(g.call("cellRudpPollCreate", 4));
    REQUIRE(g.call("cellRudpPollControl", poll, 1, client, kPollWrite | kPollFlush) == 0);
    CHECK(g.call("cellRudpPollWait", poll, kPollEvents, 4, 0) == 0);

    // Reading opens the window: every message, in order; the writer hears WRITABLE and FLUSHED.
    for (u32 i = 0; i < written; i++) {
        INFO("message " << i);
        const auto [r, got] = take(g, conn, 1000);
        REQUIRE(r == 1000);
        CHECK(got == message(i, 1000));
    }
    REQUIRE(wait_for([&] { return has_event(b, kRingB, client, kEvWritable); }));
    REQUIRE(wait_for([&] { return has_event(b, kRingB, client, kEvFlushed); }));
    CHECK(g.call("cellRudpFlush", client) == 0);
    REQUIRE(g.call("cellRudpPollWait", poll, kPollEvents, 4, k5s) == 1);
    CHECK(ps_read32(b, kPollEvents) == static_cast<u32>(client));
    CHECK(ps_read16(b, kPollEvents + 4) == (kPollWrite | kPollFlush));
    CHECK(ps_read16(b, kPollEvents + 6) == (kPollWrite | kPollFlush));
    CHECK(g.call("cellRudpGetSizeWritable", client) == 6000);
    REQUIRE(g.call("cellRudpEnd") == 0);
}

TEST_CASE("cellRudp: polling: READ, WRITE, FLUSH and ERROR; a timeout; PollCancel and PollDestroy end a wait",
    "[hle][rudp]")
{
    RudpFixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    REQUIRE(g.call("cellRudpInit", 0) == 0);
    REQUIRE(g.call("cellRudpEnableInternalIOThread", 0x8000, 1000) == 0);
    const s32 sa = socket_at(g, 5500), sb = socket_at(g, 5501);
    const s32 server = bound(g, kRingA, sa, 1), client = bound(g, kRingB, sb, 2);
    REQUIRE(g.call("cellRudpListen", server, 1) == 0);
    const s32 poll = static_cast<s32>(g.call("cellRudpPollCreate", 8));
    REQUIRE(g.call("cellRudpPollControl", poll, 1, server, kPollRead) == 0);
    sockaddr(b, kPeer, 5500);
    REQUIRE(g.call("cellRudpInitiate", client, kPeer, 16, 1) == 0);
    // A listener is readable once a connection can be accepted.
    REQUIRE(g.call("cellRudpPollWait", poll, kPollEvents, 8, k5s) == 1);
    CHECK(ps_read32(b, kPollEvents) == static_cast<u32>(server));
    CHECK(ps_read16(b, kPollEvents + 6) == kPollRead);
    const s32 conn = static_cast<s32>(g.call("cellRudpAccept", server));
    REQUIRE(conn > 0);
    REQUIRE(g.call("cellRudpPollControl", poll, 3, server, 0) == 0);
    REQUIRE(g.call("cellRudpPollControl", poll, 1, conn, kPollRead) == 0);
    REQUIRE(g.call("cellRudpPollControl", poll, 1, client, kPollWrite) == 0);
    // The client can write; the accepted context has nothing to read yet.
    REQUIRE(wait_for([&] { return state_of(g, client) == kEstablished; }));
    REQUIRE(g.call("cellRudpPollWait", poll, kPollEvents, 8, 0) == 1);
    CHECK(ps_read32(b, kPollEvents) == static_cast<u32>(client));
    CHECK(ps_read16(b, kPollEvents + 6) == kPollWrite);
    REQUIRE(g.call("cellRudpPollControl", poll, 2, client, 0) == 0); // nothing asked for of it
    CHECK(g.call("cellRudpPollWait", poll, kPollEvents, 8, 30'000) == 0); // the timeout
    // Data: READ.
    REQUIRE(put(g, client, message(1, 10)) == 10);
    REQUIRE(g.call("cellRudpPollWait", poll, kPollEvents, 8, k5s) == 1);
    CHECK(ps_read32(b, kPollEvents) == static_cast<u32>(conn));
    CHECK(ps_read16(b, kPollEvents + 6) == kPollRead);
    CHECK(take(g, conn, 64).first == 10);
    // FLUSH: everything written acknowledged.
    REQUIRE(g.call("cellRudpPollControl", poll, 2, client, kPollFlush) == 0);
    REQUIRE(g.call("cellRudpPollWait", poll, kPollEvents, 8, k5s) == 1);
    CHECK(ps_read16(b, kPollEvents + 6) == kPollFlush);
    REQUIRE(g.call("cellRudpPollControl", poll, 2, client, 0) == 0);

    // A wait ended by PollCancel, then by PollDestroy.
    for (const char* end : {"cellRudpPollCancel", "cellRudpPollDestroy"}) {
        INFO(end);
        std::atomic<lv2::PpuThread*> parked{nullptr};
        u64 answer = 0;
        std::thread waiter([&] {
            GuestThread w(f, 1, "waiter");
            parked = w.t;
            answer = w.call("cellRudpPollWait", poll, kPollEvents + 0x100, 8, k5s);
            parked = nullptr;
        });
        REQUIRE(wait_for([&] {
            lv2::PpuThread* p = parked.load();
            return p && p->waiter.waiting_on().kind == lv2::WaitKind::Net;
        }));
        REQUIRE(g.call(end, poll) == 0);
        waiter.join();
        CHECK(answer == code(CANCELLED));
    }

    // ERROR: a connection reset (a RST made here, given to the library as the peer's).
    const s32 poll2 = static_cast<s32>(g.call("cellRudpPollCreate", 2));
    REQUIRE(g.call("cellRudpPollControl", poll2, 1, client, 0) == 0); // ERROR comes whatever is asked
    rudp::Header h;
    h.type = rudp::Type::Rst;
    h.reason = rudp::kRstReset;
    h.src = 1;
    h.dst = 2;
    rudp::encode(h, b + kIn);
    sockaddr(b, kAddr, 5500);
    REQUIRE(g.call("cellRudpNetReceived", sb, kIn, 32, kAddr, 16) == 0);
    REQUIRE(g.call("cellRudpPollWait", poll2, kPollEvents, 2, k5s) == 1);
    CHECK(ps_read16(b, kPollEvents + 6) == kPollError);
    CHECK(state_of(g, client) == kClosed);
    CHECK(get_opt(g, client, kLastError) == static_cast<u64>(static_cast<s64>(static_cast<s32>(CONN_RESET))));
    CHECK(has_event(b, kRingB, client, kEvError, CONN_RESET));
    CHECK(has_event(b, kRingB, client, kEvClosed, CONN_RESET));
    CHECK(take(g, client, 64).first == code(CONN_RESET));
    CHECK(put(g, client, message(1, 10)) == code(CONN_RESET));
    REQUIRE(g.call("cellRudpGetOption", client, kLastError, kArgs, 4) == 0);
    CHECK(ps_read32(b, kArgs) == CONN_RESET);
    CHECK(g.call("cellRudpGetOption", client, kLastError, kArgs, 2) == code(BUFFER_TOO_SMALL));
    REQUIRE(g.call("cellRudpEnd") == 0);
}

TEST_CASE("cellRudp: connections that fail: refused, timed out, keep-alive, mismatched", "[hle][rudp]")
{
    RudpFixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    REQUIRE(g.call("cellRudpInit", 0) == 0);
    REQUIRE(g.call("cellRudpEnableInternalIOThread", 0x8000, 1000) == 0);
    const s32 sa = socket_at(g, 5600), sb = socket_at(g, 5601);

    // Nobody at the vport: refused.
    const s32 lone = bound(g, kRingA, sa, 1), c1 = bound(g, kRingB, sb, 1);
    sockaddr(b, kPeer, 5600);
    REQUIRE(g.call("cellRudpInitiate", c1, kPeer, 16, 9) == 0);
    REQUIRE(wait_for([&] { return state_of(g, c1) == kClosed; }));
    CHECK(get_opt(g, c1, kLastError) == static_cast<u64>(static_cast<s64>(static_cast<s32>(CONN_REFUSED))));
    CHECK(has_event(b, kRingB, c1, kEvError, CONN_REFUSED));
    CHECK(stat(g, kStatFailedRefused) == 1);
    REQUIRE(g.call("cellRudpTerminate", c1) == 0);

    // Nobody at the address: the connection timeout.
    const s32 c2 = bound(g, kRingB, sb, 2);
    REQUIRE(set_opt(g, c2, kConnectionTimeout, 150'000, 8) == 0);
    sockaddr(b, kPeer, 5999);
    REQUIRE(g.call("cellRudpInitiate", c2, kPeer, 16, 1) == 0);
    REQUIRE(wait_for([&] { return state_of(g, c2) == kClosed; }));
    CHECK(get_opt(g, c2, kLastError) == static_cast<u64>(static_cast<s64>(static_cast<s32>(CONN_TIMEOUT))));
    CHECK(has_event(b, kRingB, c2, kEvClosed, CONN_TIMEOUT));
    REQUIRE(g.call("cellRudpTerminate", c2) == 0);

    // A stream meeting messages, then different delivery guarantees: TRANSPORT_TYPE and QUALITY_LEVEL mismatches.
    REQUIRE(g.call("cellRudpListen", lone, 2) == 0);
    REQUIRE(set_opt(g, lone, kStream, 1) == 0);
    sockaddr(b, kPeer, 5600);
    const s32 c3 = bound(g, kRingB, sb, 3);
    REQUIRE(g.call("cellRudpInitiate", c3, kPeer, 16, 1) == 0);
    REQUIRE(wait_for([&] { return state_of(g, c3) == kClosed; }));
    CHECK(get_opt(g, c3, kLastError) == static_cast<u64>(static_cast<s64>(static_cast<s32>(TRANSPORT_MISMATCH))));
    REQUIRE(set_opt(g, lone, kStream, 0) == 0);
    REQUIRE(set_opt(g, lone, kOrder, 0) == 0);
    const s32 c4 = bound(g, kRingB, sb, 4);
    REQUIRE(g.call("cellRudpInitiate", c4, kPeer, 16, 1) == 0);
    REQUIRE(wait_for([&] { return state_of(g, c4) == kClosed; }));
    CHECK(get_opt(g, c4, kLastError) == static_cast<u64>(static_cast<s64>(static_cast<s32>(QUALITY_MISMATCH))));
    REQUIRE(set_opt(g, lone, kOrder, 1) == 0);

    // Another protocol version: the SYN is answered with a RST that says so.
    rudp::set_send_filter([](rudp::Packet p, std::vector<rudp::Packet>& send) {
        if (p.bytes.size() >= 32 && p.bytes[3] == static_cast<u8>(rudp::Type::Syn)) p.bytes[2] = 2;
        send.push_back(std::move(p));
    });
    const s32 c5 = bound(g, kRingB, sb, 5);
    REQUIRE(g.call("cellRudpInitiate", c5, kPeer, 16, 1) == 0);
    REQUIRE(wait_for([&] { return state_of(g, c5) == kClosed; }));
    CHECK(get_opt(g, c5, kLastError) == static_cast<u64>(static_cast<s64>(static_cast<s32>(VERSION_MISMATCH))));
    rudp::set_send_filter(nullptr);

    // Keep-alive: an established connection whose packets stop arriving ends on both sides.
    const s32 c6 = bound(g, kRingB, sb, 6);
    for (const s32 x : {lone, c6}) {
        REQUIRE(set_opt(g, x, kKeepAliveInterval, 20'000, 8) == 0);
        REQUIRE(set_opt(g, x, kKeepAliveTimeout, 200'000, 8) == 0);
    }
    REQUIRE(g.call("cellRudpInitiate", c6, kPeer, 16, 1) == 0);
    const s32 conn = static_cast<s32>(g.call("cellRudpAccept", lone));
    REQUIRE(conn > 0);
    REQUIRE(wait_for([&] { return state_of(g, c6) == kEstablished; }));
    std::this_thread::sleep_for(300ms); // probes keep it up meanwhile
    CHECK(state_of(g, c6) == kEstablished);
    CHECK(state_of(g, conn) == kEstablished);
    rudp::set_send_filter([](rudp::Packet, std::vector<rudp::Packet>&) {}); // the network goes
    REQUIRE(wait_for([&] { return state_of(g, c6) == kClosed && state_of(g, conn) == kClosed; }));
    CHECK(get_opt(g, c6, kLastError) == static_cast<u64>(static_cast<s64>(static_cast<s32>(KEEP_ALIVE_FAILURE))));
    CHECK(has_event(b, kRingA, conn, kEvError, KEEP_ALIVE_FAILURE));
    rudp::set_send_filter(nullptr);
    CHECK(stat(g, kStatFailed) >= 5);
    REQUIRE(g.call("cellRudpEnd") == 0);
}

TEST_CASE("cellRudp: the game's own I/O: ProcessEvents without the I/O thread; RAW sockets and NetReceived",
    "[hle][rudp]")
{
    RudpFixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    REQUIRE(g.call("cellRudpInit", 0) == 0);

    // NORMAL sockets, no I/O thread: nothing happens until the game calls ProcessEvents.
    const s32 sa = socket_at(g, 5700), sb = socket_at(g, 5701);
    const s32 server = bound(g, kRingA, sa, 1), client = bound(g, kRingB, sb, 2);
    REQUIRE(g.call("cellRudpListen", server, 1) == 0);
    REQUIRE(set_opt(g, server, kNonblock, 1) == 0);
    sockaddr(b, kPeer, 5700);
    REQUIRE(g.call("cellRudpInitiate", client, kPeer, 16, 1) == 0);
    std::this_thread::sleep_for(20ms);
    CHECK(g.call("cellRudpAccept", server) == code(WOULDBLOCK));
    CHECK(state_of(g, client) == kSynSent);
    s32 conn = 0;
    REQUIRE(wait_for([&] {
        REQUIRE(g.call("cellRudpProcessEvents", 1'000) == 0);
        if (conn <= 0) conn = static_cast<s32>(g.call("cellRudpAccept", server));
        return conn > 0 && state_of(g, client) == kEstablished;
    }));
    CHECK(has_event(b, kRingB, client, kEvEstablished)); // on this thread, in ProcessEvents
    REQUIRE(set_opt(g, conn, kNonblock, 1) == 0);
    for (u32 i = 0; i < 30; i++) REQUIRE(put(g, client, message(i, 1000)) == 1000);
    for (u32 i = 0; i < 30; i++) {
        INFO("message " << i);
        std::pair<u64, std::vector<u8>> got;
        REQUIRE(wait_for([&] {
            g.call("cellRudpProcessEvents", 0);
            got = take(g, conn, 1000);
            return got.first != code(WOULDBLOCK);
        }));
        CHECK(got.second == message(i, 1000));
    }
    REQUIRE(g.call("cellRudpTerminate", server) == 0);
    REQUIRE(g.call("cellRudpTerminate", conn) == 0);
    REQUIRE(g.call("cellRudpTerminate", client) == 0);
    REQUIRE(wait_for([&] { // both ends' FINs, sent and acknowledged as ProcessEvents runs
        REQUIRE(g.call("cellRudpProcessEvents", 1'000) == 0);
        return stat(g, kStatCurrent) == 0;
    }));

    // RAW sockets: the handler sends each packet (with sendto, here), the game reads its sockets and hands the
    // datagrams over with NetReceived; ProcessEvents runs the timers. A socket the last context let go: released.
    REQUIRE(g.call("cellRudpSetEventHandler", kRawHandler, kReleased) == 0);
    const s32 ra = socket_at(g, 5710), rb = socket_at(g, 5711);
    const s32 x = bound(g, kRingA, ra, 1, 1), y = bound(g, kRingB, rb, 1, 1);
    sockaddr(b, kPeer, 5711);
    sockaddr(b, kPeer + 16, 5710);
    REQUIRE(set_opt(g, x, kNonblock, 1) == 0);
    REQUIRE(set_opt(g, y, kNonblock, 1) == 0);
    REQUIRE(g.call("cellRudpActivate", x, kPeer, 16) == 0);
    REQUIRE(g.call("cellRudpInitiate", y, kPeer + 16, 16, 1) == 0);
    // The game's receive loop: each socket drained into NetReceived.
    const auto pump = [&] {
        for (const s32 s : {ra, rb}) {
            for (;;) {
                ps_write32(b, kArgs + 0x80, 16);
                const s32 n = sc(g, 707, {static_cast<u64>(s), kRx, 65536, 0x80, kAddr, kArgs + 0x80});
                if (n <= 0) break;
                REQUIRE(g.call("cellRudpNetReceived", s, kRx, n, kAddr, 16) == 0);
            }
        }
        REQUIRE(g.call("cellRudpProcessEvents", 0) == 0);
    };
    REQUIRE(wait_for([&] {
        pump();
        return state_of(g, x) == kEstablished && state_of(g, y) == kEstablished;
    }));
    REQUIRE(put(g, x, message(5, 700)) == 700);
    std::pair<u64, std::vector<u8>> got;
    REQUIRE(wait_for([&] {
        pump();
        got = take(g, y, 2048);
        return got.first != code(WOULDBLOCK);
    }));
    CHECK(got.second == message(5, 700));
    // The RAW sockets are not the library's to read: nothing of them is taken without NetReceived.
    REQUIRE(put(g, y, message(6, 10)) == 10);
    for (u32 i = 0; i < 5; i++) REQUIRE(g.call("cellRudpProcessEvents", 2'000) == 0);
    CHECK(take(g, x, 2048).first == code(WOULDBLOCK));
    REQUIRE(wait_for([&] {
        pump();
        return g.call("cellRudpGetSizeReadable", x) == 10;
    }));
    CHECK(load32_acquire(b, kReleased) == 0);
    // The end: x's FIN, y's after it; each socket released once its last connection has both ends acknowledged.
    REQUIRE(g.call("cellRudpTerminate", x) == 0);
    REQUIRE(wait_for([&] {
        pump();
        return state_of(g, y) == kCloseWait;
    }));
    CHECK(load32_acquire(b, kReleased) == 0); // x lingers for y's FIN
    REQUIRE(g.call("cellRudpTerminate", y) == 0);
    REQUIRE(wait_for([&] {
        pump();
        return load32_acquire(b, kReleased) == 2;
    }));
    const s32 last = static_cast<s32>(ps_read32(b, kReleased + 4));
    CHECK((last == ra || last == rb));
    CHECK(stat(g, kStatCurrent) == 0);
    REQUIRE(g.call("cellRudpEnd") == 0);
}

TEST_CASE("cellRudp: End resets the connections, ends the waits and gives no ID twice", "[hle][rudp]")
{
    RudpFixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    REQUIRE(g.call("cellRudpInit", 0) == 0);
    REQUIRE(g.call("cellRudpEnableInternalIOThread", 0x8000, 1000) == 0);
    const s32 sa = socket_at(g, 5800), sb = socket_at(g, 5801);
    const s32 server = bound(g, kRingA, sa, 1), client = bound(g, kRingB, sb, 2);
    REQUIRE(g.call("cellRudpListen", server, 1) == 0);
    sockaddr(b, kPeer, 5800);
    REQUIRE(g.call("cellRudpInitiate", client, kPeer, 16, 1) == 0);
    const s32 conn = static_cast<s32>(g.call("cellRudpAccept", server));
    REQUIRE(conn > 0);
    std::atomic<lv2::PpuThread*> parked{nullptr};
    u64 answer = 0;
    std::thread reader([&] {
        GuestThread r(f, 1, "reader");
        parked = r.t;
        answer = r.call("cellRudpRead", conn, kIn, 64, 0, 0);
        parked = nullptr;
    });
    REQUIRE(wait_for([&] {
        lv2::PpuThread* p = parked.load();
        return p && p->waiter.waiting_on().kind == lv2::WaitKind::Net;
    }));
    REQUIRE(g.call("cellRudpEnd") == 0);
    reader.join();
    CHECK(answer == code(NOT_INITIALIZED));
    // A new library: nothing of the old one; the old IDs are not handed out again.
    REQUIRE(g.call("cellRudpInit", 0) == 0);
    const s32 fresh = create(g, kRingC);
    CHECK(fresh > conn);
    CHECK(g.call("cellRudpTerminate", client) == code(INVALID_CONTEXT_ID));
    CHECK(stat(g, kStatCurrent) == 1);
    REQUIRE(g.call("cellRudpEnd") == 0);
}

TEST_CASE("cellRudp: a socket the game closes under its contexts is no longer polled, and read again once reused",
    "[hle][rudp]")
{
    RudpFixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    REQUIRE(g.call("cellRudpInit", 0) == 0);
    REQUIRE(g.call("cellRudpEnableInternalIOThread", 0x8000, 1000) == 0);
    const s32 sa = socket_at(g, 6100), sb = socket_at(g, 6101), sx = socket_at(g, 6102);
    const s32 server = bound(g, kRingA, sa, 1), client = bound(g, kRingB, sb, 2);
    bound(g, kRingC, sx, 3); // left bound on the closed socket
    REQUIRE(g.call("cellRudpListen", server, 2) == 0);
    sockaddr(b, kPeer, 6100);
    REQUIRE(g.call("cellRudpInitiate", client, kPeer, 16, 1) == 0);
    const s32 conn = static_cast<s32>(g.call("cellRudpAccept", server));
    REQUIRE(conn > 0);
    constexpr std::string_view kGone = "was closed while contexts use it";
    {
        // lv2 answers POLLNVAL for it, at once, in every poll: the library says so once and polls the others only.
        LogLines warned("cellRudp", log::Level::Warn);
        REQUIRE(sc(g, 714, {static_cast<u64>(sx)}) == 0);
        REQUIRE(wait_for([&] { return warned.count(kGone) > 0; }));
        std::this_thread::sleep_for(50ms);
        CHECK(warned.count(kGone) == 1);
    }
    {
        // A quiet while: the I/O thread waits in each poll for its tick (some 20 polls), it does not spin on the
        // closed socket's POLLNVAL (thousands of polls before the fix).
        LogLines polls("sys_net", log::Level::Debug);
        std::this_thread::sleep_for(200ms);
        const usize all = polls.count("socketpoll(");
        CHECK(all > 0);
        CHECK(all < 100);
        CHECK(polls.count("socketpoll(nfds 2,") == all);
    }
    REQUIRE(put(g, client, message(1, 10)) == 10);
    CHECK(take(g, conn, 64).second == message(1, 10));
    // lv2 hands the closed socket's number out again once its range is used up: a context bound to that new socket is
    // read, its connection works.
    s32 again = -1;
    {
        LogLines quiet("sys_net", log::Level::Info); // lv2's 2,000 lines kept out of the output
        for (u32 i = 0; i < 1100 && again < 0; i++) {
            const s32 s = sc(g, 713, {2, static_cast<u64>(kSockDgram), 0});
            REQUIRE(s >= 0);
            if (s == sx) again = s;
            else REQUIRE(sc(g, 714, {static_cast<u64>(s)}) == 0);
        }
    }
    REQUIRE(again == sx);
    sockaddr(b, kAddr, 6103);
    REQUIRE(sc(g, 701, {static_cast<u64>(again), kAddr, 16}) == 0);
    const s32 second = bound(g, kRingC, again, 4);
    REQUIRE(g.call("cellRudpInitiate", second, kPeer, 16, 1) == 0);
    const s32 conn2 = static_cast<s32>(g.call("cellRudpAccept", server));
    REQUIRE(conn2 > 0);
    REQUIRE(put(g, second, message(2, 20)) == 20);
    CHECK(take(g, conn2, 64).second == message(2, 20));
    REQUIRE(put(g, conn2, message(3, 30)) == 30);
    CHECK(take(g, second, 64).second == message(3, 30));
    REQUIRE(g.call("cellRudpEnd") == 0);
}

TEST_CASE("cellRudp: thousands of small messages in flight at once, each once and in order", "[hle][rudp]")
{
    RudpFixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    REQUIRE(g.call("cellRudpInit", 0) == 0);
    REQUIRE(g.call("cellRudpEnableInternalIOThread", 0x8000, 1000) == 0);
    const s32 sa = socket_at(g, 6000), sb = socket_at(g, 6001);
    const s32 server = bound(g, kRingA, sa, 1), client = bound(g, kRingB, sb, 2);
    REQUIRE(g.call("cellRudpListen", server, 1) == 0);
    sockaddr(b, kPeer, 6000);
    REQUIRE(g.call("cellRudpInitiate", client, kPeer, 16, 1) == 0);
    const s32 conn = static_cast<s32>(g.call("cellRudpAccept", server));
    REQUIRE(conn > 0);
    constexpr u32 kCount = 5000;
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kCount; i++) {
        std::vector<u8> m(8);
        for (u32 k = 0; k < 8; k++) m[k] = static_cast<u8>((i >> (8 * (k % 4))) + k);
        REQUIRE(put(g, client, m) == 8);
    }
    for (u32 i = 0; i < kCount; i++) {
        INFO("message " << i);
        const auto [r, got] = take(g, conn, 64);
        REQUIRE(r == 8);
        for (u32 k = 0; k < 8; k++) CHECK(got[k] == static_cast<u8>((i >> (8 * (k % 4))) + k));
    }
    REQUIRE(g.call("cellRudpFlush", client) == 0);
    CHECK(std::chrono::steady_clock::now() - start < 20s); // no stall: well under a second here
    REQUIRE(g.call("cellRudpEnd") == 0);
}

TEST_CASE("cellRudp: a handler may call the library; ProcessEvents from inside the library's I/O is THREAD_IN_USE",
    "[hle][rudp]")
{
    RudpFixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    write_nested_handler(f);
    REQUIRE(g.call("cellRudpInit", 0) == 0);
    for (const bool io : {true, false}) {
        INFO((io ? "the I/O thread" : "ProcessEvents on the game's thread"));
        if (io) REQUIRE(g.call("cellRudpEnableInternalIOThread", 0x8000, 1000) == 0);
        ps_write32(b, kRingC + 0xE00, 0);
        ps_write32(b, kRingC + 0xE04, 0);
        const s32 sa = socket_at(g, io ? 5900 : 5902), sb = socket_at(g, io ? 5901 : 5903);
        const s32 server = bound(g, kRingA, sa, 1);
        REQUIRE(g.call("cellRudpCreateContext", kNestedHandler, kRingC, kArgs) == 0);
        const s32 client = static_cast<s32>(ps_read32(b, kArgs));
        REQUIRE(g.call("cellRudpBind", client, sb, 2, 0) == 0);
        REQUIRE(g.call("cellRudpListen", server, 1) == 0);
        sockaddr(b, kPeer, io ? 5900 : 5902);
        REQUIRE(g.call("cellRudpInitiate", client, kPeer, 16, 1) == 0);
        REQUIRE(wait_for([&] {
            if (!io) REQUIRE(g.call("cellRudpProcessEvents", 1'000) == 0);
            return load32_acquire(b, kRingC + 0xE04) >= 1; // ESTABLISHED
        }));
        CHECK(ps_read32(b, kRingC + 0xE00) == THREAD_IN_USE);
        CHECK(state_of(g, client) == kEstablished);
        REQUIRE(g.call("cellRudpEnd") == 0);
        REQUIRE(g.call("cellRudpInit", 0) == 0);
    }
    REQUIRE(g.call("cellRudpEnd") == 0);
}
