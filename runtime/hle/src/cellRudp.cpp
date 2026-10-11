// cellRudp: Sony's reliable UDP for games' peer-to-peer play (librudp), pspocket's own (D-607, D-630 to D-633):
// contexts that are connections over the game's UDP or UDP P2P sockets, with sequence numbers, acknowledgements,
// retransmission after a timeout, flow control to the windows the options set, segments of the maximum segment size,
// keep-alive, ordered or unordered and reliable or unreliable delivery as the options say, and the events the game's
// handlers get; several contexts share a socket, told apart by virtual ports (vports).
//
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellRudp.cpp and cellRudp.h: the error codes, the option numbers, the poll
// event flags, CellRudpAllocator, CellRudpEventHandler's arguments, Init, End and SetEventHandler. RPCS3 has stubs for
// the rest and no signatures. What is not there was decided here (D-630) from the library's names, its error codes and
// options, and the argument lists its successors (the PS Vita's and PS4's RUDP: the same functions) are recalled to
// have, no document of them being at hand: unverified, each choice listed in D-630 with what to check against Sony's
// librudp on the Mac (tests/hle/test_rudp_compare.cpp, D-633).
// The wire format is pspocket's own (rudp.hpp, D-631): Sony's is not known, so this library talks only to pspocket's.
// pspocket's network is a console without a link (D-523): datagrams go between the game's own sockets.
//
// The library stays firmware-first (D-563): where the system software is installed a game gets Sony's librudp.
//
// I/O (D-632). A context is bound to a socket the game made (cellRudpBind) in one of two modes:
// - CELL_RUDP_MUXMODE_NORMAL: the library does the socket's I/O. It sends with sendto; it receives on the internal I/O
//   thread (cellRudpEnableInternalIOThread: a PPU thread of the game's process) or, without one, in
//   cellRudpProcessEvents, which also runs the timers (retransmission, keep-alive, connection and close timeouts).
// - CELL_RUDP_MUXMODE_RAW: the game does it: the library hands each packet to the event handler (CELL_RUDP_EVENT_SEND)
//   to send, and takes what the game received with cellRudpNetReceived (in either mode).
// The context handler (CellRudpContextEventHandler: ctx_id, event_id, error_code, arg) and the event handler run on
// the thread that processed what caused them: the I/O thread, or the game's in NetReceived or ProcessEvents. No lock of
// the library is held while guest code runs, so a handler may call the library.

#include "rudp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <format>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/object.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

using rudp::Header;
using rudp::kHeaderSize;
using rudp::kSynSize;
using rudp::Type;

constexpr std::string_view kModule = "cellRudp";
constexpr const char* kChan = "cellRudp";

// ------------------------------------------------------------------------------------------------- constants

// RPCS3 cellRudp.h.
enum : u32 {
    CELL_RUDP_ERROR_NOT_INITIALIZED = 0x80770001,
    CELL_RUDP_ERROR_ALREADY_INITIALIZED = 0x80770002,
    CELL_RUDP_ERROR_INVALID_CONTEXT_ID = 0x80770003,
    CELL_RUDP_ERROR_INVALID_ARGUMENT = 0x80770004,
    CELL_RUDP_ERROR_INVALID_OPTION = 0x80770005,
    CELL_RUDP_ERROR_INVALID_MUXMODE = 0x80770006,
    CELL_RUDP_ERROR_MEMORY = 0x80770007,
    CELL_RUDP_ERROR_INTERNAL = 0x80770008,
    CELL_RUDP_ERROR_CONN_RESET = 0x80770009,
    CELL_RUDP_ERROR_CONN_REFUSED = 0x8077000a,
    CELL_RUDP_ERROR_CONN_TIMEOUT = 0x8077000b,
    CELL_RUDP_ERROR_CONN_VERSION_MISMATCH = 0x8077000c,
    CELL_RUDP_ERROR_CONN_TRANSPORT_TYPE_MISMATCH = 0x8077000d,
    CELL_RUDP_ERROR_QUALITY_LEVEL_MISMATCH = 0x8077000e,
    CELL_RUDP_ERROR_THREAD = 0x8077000f,
    CELL_RUDP_ERROR_THREAD_IN_USE = 0x80770010,
    CELL_RUDP_ERROR_NOT_ACCEPTABLE = 0x80770011,
    CELL_RUDP_ERROR_MSG_TOO_LARGE = 0x80770012,
    CELL_RUDP_ERROR_NOT_BOUND = 0x80770013,
    CELL_RUDP_ERROR_CANCELLED = 0x80770014,
    CELL_RUDP_ERROR_INVALID_VPORT = 0x80770015,
    CELL_RUDP_ERROR_WOULDBLOCK = 0x80770016,
    CELL_RUDP_ERROR_VPORT_IN_USE = 0x80770017,
    CELL_RUDP_ERROR_VPORT_EXHAUSTED = 0x80770018,
    CELL_RUDP_ERROR_INVALID_SOCKET = 0x80770019,
    CELL_RUDP_ERROR_BUFFER_TOO_SMALL = 0x8077001a,
    CELL_RUDP_ERROR_MSG_MALFORMED = 0x8077001b,
    CELL_RUDP_ERROR_ADDR_IN_USE = 0x8077001c,
    CELL_RUDP_ERROR_ALREADY_BOUND = 0x8077001d,
    CELL_RUDP_ERROR_ALREADY_EXISTS = 0x8077001e,
    CELL_RUDP_ERROR_INVALID_POLL_ID = 0x8077001f,
    CELL_RUDP_ERROR_TOO_MANY_CONTEXTS = 0x80770020,
    CELL_RUDP_ERROR_IN_PROGRESS = 0x80770021,
    CELL_RUDP_ERROR_NO_EVENT_HANDLER = 0x80770022,
    CELL_RUDP_ERROR_PAYLOAD_TOO_LARGE = 0x80770023,
    CELL_RUDP_ERROR_END_OF_DATA = 0x80770024,
    CELL_RUDP_ERROR_ALREADY_ESTABLISHED = 0x80770025,
    CELL_RUDP_ERROR_KEEP_ALIVE_FAILURE = 0x80770026,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// The context options (RPCS3 cellRudp.h CELL_RUDP_OPTION_*; 12 and 13 have no name there and are refused).
enum : s32 {
    kOptMaxPayload = 1,
    kOptSndbuf = 2,
    kOptRcvbuf = 3,
    kOptNodelay = 4,
    kOptDeliveryCritical = 5,
    kOptOrderCritical = 6,
    kOptNonblock = 7,
    kOptStream = 8,
    kOptConnectionTimeout = 9,
    kOptCloseWaitTimeout = 10,
    kOptAggregationTimeout = 11,
    kOptLastError = 14,
    kOptReadTimeout = 15,
    kOptWriteTimeout = 16,
    kOptFlushTimeout = 17,
    kOptKeepAliveInterval = 18,
    kOptKeepAliveTimeout = 19,
};

// A context's state (CellRudpContextStatus::state, D-630).
enum : u32 {
    kStateIdle = 0,
    kStateClosed = 1,
    kStateSynSent = 2,
    kStateSynRcvd = 3,
    kStateEstablished = 4,
    kStateCloseWait = 5,
};

// The context handler's events and the event handler's (D-630).
enum : s32 {
    kEventClosed = 1,
    kEventEstablished = 2,
    kEventError = 3,
    kEventWritable = 4,
    kEventReadable = 5,
    kEventFlushed = 6,
};
enum : s32 { kEventSend = 1, kEventSocketReleased = 2 };

// Polling (RPCS3 cellRudp.h CELL_RUDP_POLL_EV_*; the operations D-630).
enum : u16 { kPollRead = 0x0001, kPollWrite = 0x0002, kPollFlush = 0x0004, kPollError = 0x0008 };
constexpr u16 kPollEvents = kPollRead | kPollWrite | kPollFlush | kPollError;
enum : s32 { kPollAdd = 1, kPollModify = 2, kPollRemove = 3 };

// Read and Write flags (D-630): DONTWAIT, LATENCY_CRITICAL, ALIGN_32, ALIGN_64, WITH_TIMESTAMP.
enum : u8 { kMsgDontwait = 0x01, kMsgLatencyCritical = 0x08 };

enum : u8 { kMuxNormal = 0, kMuxRaw = 1 };

// lv2's network calls (RPCS3 lv2/sys_net.h; the socket information as pspocket's lv2 gives it, D-523).
constexpr u32 kSysGetsockname = 704, kSysRecvfrom = 707, kSysSendto = 710, kSysPoll = 715, kSysInfoctl = 722;
constexpr u32 kInfoctlSockinfoEx = 11, kSockinfoBlock = 32, kSockinfoExSize = 80;
constexpr s32 kSockDgram = 2, kSockDgramP2p = 6;
constexpr s32 kMsgDontwaitNet = 0x80; // SYS_NET_MSG_DONTWAIT
constexpr u16 kPollin = 0x0001, kPollnval = 0x0020; // SYS_NET_POLLIN, SYS_NET_POLLNVAL
constexpr u32 kSockaddrSize = 16;
constexpr u32 kLoopback = 0x7F000001;

// Limits and defaults (D-630: Sony's are not known; to compare on the Mac).
constexpr u16 kDefaultMss = 1400, kMinMss = 128; // a segment: header and payload, one UDP datagram
constexpr u32 kMaxDatagram = 65507;             // what UDP carries
constexpr u32 kDefaultBuffer = 65536, kMinBuffer = 256, kMaxBuffer = 16u << 20;
constexpr u32 kMaxContexts = 256, kMaxPolls = 64, kMaxPollSize = 1024;
constexpr u32 kDefaultBacklog = 8, kMaxBacklog = 64;
constexpr u32 kRecordSize = 0x100; // each context's record from the allocator
constexpr u64 kDefaultConnectionTimeout = 30'000'000, kDefaultCloseWaitTimeout = 1'000'000,
              kDefaultAggregationTimeout = 10'000, kDefaultKeepAliveInterval = 1'000'000,
              kDefaultKeepAliveTimeout = 10'000'000;
constexpr u64 kInitialRto = 200'000, kMinRto = 20'000, kMaxRto = 1'000'000; // microseconds
constexpr double kInitialCwnd = 4, kInitialSsthresh = 32, kMaxCwnd = 128; // segments in flight
constexpr u64 kForever = 1ull << 40;   // a timeout this long or longer has no end
constexpr u64 kIoTick = 10'000;        // the I/O thread looks at its sockets and timers at least this often
constexpr u32 kRxSize = 65536;         // a datagram received
constexpr u32 kPollSlots = 64;         // sockets the I/O thread waits on at once
constexpr u32 kIoBlockSize = kRxSize + 32 + 8 * kPollSlots;
constexpr u32 kStackScratch = 4096;    // packets up to this size are staged on the sending thread's stack
constexpr u32 kDrainLimit = 512;       // datagrams taken from one socket in one go

// CellRudpStatus (D-630): 50 counters of 8 bytes, in this order.
enum Stat : u32 {
    kSentUdpBytes, kRcvdUdpBytes, kSentUdpPackets, kRcvdUdpPackets, kSentUserBytes, kSentUserPackets,
    kSentLatencyCriticalPackets, kRcvdUserBytes, kRcvdUserPackets, kRcvdLatencyCriticalPackets, kSentSynPackets,
    kRcvdSynPackets, kSentUsrPackets, kRcvdUsrPackets, kSentPrbPackets, kRcvdPrbPackets, kSentRstPackets,
    kRcvdRstPackets, kLostPackets, kRetransmittedPackets, kReorderedPackets, kCurrentContexts,
    kQualityLevels, // per quality level 1 to 4: sent bytes, received bytes, sent packets, received packets
    kAllocs = kQualityLevels + 16, kFrees, kMemCurrent, kMemPeak, kEstablishedConnections, kFailedConnections,
    kFailedReset, kFailedRefused, kFailedTimeout, kFailedVersion, kFailedTransport, kFailedQuality, kStatCount
};
constexpr u32 kStatusSize = 8 * kStatCount;
constexpr u32 kContextStatusSize = 48;
constexpr u32 kReadInfoSize = 12, kPollEventSize = 8;

u64 now_us() { return lv2::guest_system_time_us(); }
bool seq_lt(u32 a, u32 b) { return static_cast<s32>(a - b) < 0; }
bool seq_gt(u32 a, u32 b) { return static_cast<s32>(a - b) > 0; }

// ------------------------------------------------------------------------------------------------- state

// A peer's address as a game gives it (a sys_net_sockaddr_in, or _in_p2p with the P2P socket's virtual port).
struct Endpoint {
    std::array<u8, 16> raw{}; // sendto takes it as it is
    u32 addr = 0;             // 0.0.0.0 as 127.0.0.1, where lv2 delivers it
    u16 port = 0;
    u16 p2p = 0; // sin_vport: P2P sockets only
};

Endpoint endpoint_of(const u8* sa)
{
    Endpoint e;
    std::memcpy(e.raw.data(), sa, kSockaddrSize);
    e.port = ps_ld16(sa + 2);
    e.addr = ps_ld32(sa + 4);
    if (!e.addr) e.addr = kLoopback;
    e.p2p = ps_ld16(sa + 8);
    return e;
}

bool same_peer(const Endpoint& a, const Endpoint& b, bool p2p)
{
    return a.addr == b.addr && a.port == b.port && (!p2p || a.p2p == b.p2p);
}

struct Options {
    u32 max_payload = 0; // set from the segment size when the context is made
    u32 sndbuf = kDefaultBuffer, rcvbuf = kDefaultBuffer;
    bool nodelay = false, delivery = true, order = true, nonblock = false, stream = false;
    u64 connection_timeout = kDefaultConnectionTimeout, close_wait_timeout = kDefaultCloseWaitTimeout,
        aggregation_timeout = kDefaultAggregationTimeout, read_timeout = 0, write_timeout = 0, flush_timeout = 0,
        keep_alive_interval = kDefaultKeepAliveInterval, keep_alive_timeout = kDefaultKeepAliveTimeout;
};

// What a connection carries, fixed when it starts: a byte stream or messages; delivery and order guaranteed or not
// (a stream always both).
struct Params {
    bool stream = false, reliable = true, ordered = true;
};
Params params_of(const Options& o) { return {o.stream, o.stream || o.delivery, o.stream || o.order}; }
u32 quality_of(const Params& p) { return (p.reliable ? 0u : 2u) + (p.ordered ? 0u : 1u); } // level - 1

// A segment sent or to send: DATA (a message, or a stream's bytes) or FIN.
struct Segment {
    u32 seq = 0;
    Type type = Type::Data;
    u8 flags = 0;
    std::vector<u8> data;
    u64 offset = 0; // the connection's payload bytes before it
    u64 first_sent = 0, last_sent = 0;
    u32 sends = 0, stamp = 0;
    bool sacked = false;
};

// Data received, waiting for cellRudpRead.
struct Chunk {
    std::vector<u8> data;
    usize read = 0; // a stream's bytes already read
    u32 seq = 0, stamp = 0;
    u8 flags = 0, retries = 0;
    bool fin = false; // the peer's end (no data)
};

struct Context {
    s32 id = 0; // 0: none (a listener's connection not accepted yet, or one the game terminated, lingering)
    u32 handler = 0, arg = 0;
    u32 record = 0, record_free = 0; // the context's record and the app_free it goes back to (0: the main area)
    bool record_own = false;
    u32 state = kStateIdle;
    Options opt;
    // The socket.
    bool bound = false;
    s32 soc = -1;
    u16 vport = 0;
    u8 mux = kMuxNormal;
    // How it connects.
    bool listening = false, activated = false, lingering = false;
    u32 backlog = 0;
    std::weak_ptr<Context> parent;
    s32 parent_id = 0;
    u32 children = 0;
    std::vector<std::shared_ptr<Context>> pending; // a listener's connections not accepted yet
    bool has_peer = false;
    Endpoint peer;
    u16 peer_vport = 0;
    // The connection.
    Params p;
    u16 mss = 0;      // this side's segment size (header and payload); the peer's caps it
    u32 payload = 0;  // a segment's payload
    u32 iss = 0, irs = 0;
    u32 snd_una = 0, snd_nxt = 0;
    u64 snd_offset = 0; // payload bytes given sequence numbers so far
    // The segments not acknowledged yet, in order: the first `sent` of them went out at least once (segments go out
    // first in order), `flight` of those the peer has not acknowledged selectively; `queued_bytes` their payload and
    // stream_out's.
    std::deque<Segment> sndq;
    usize sent = 0, queued_bytes = 0;
    u32 flight = 0;
    std::deque<u8> stream_out; // a stream's bytes not in a segment yet
    bool fin_wanted = false, fin_queued = false, flush_now = false;
    u64 nagle_since = 0;
    u32 peer_window = 0;
    double cwnd = kInitialCwnd, ssthresh = kInitialSsthresh;
    u64 srtt = 0, rttvar = 0, rto = kInitialRto;
    bool rtt_valid = false;
    u64 rto_deadline = 0, persist_deadline = 0;
    // Loss detection (RACK, RFC 8985, simplified): the latest sending of a segment the peer has; a recovery episode
    // (the window shrinks once in one) until snd_una passes `recover`.
    u64 rack_xmit = 0;
    bool recovery = false;
    u32 recover = 0;
    u32 rcv_nxt = 0, highest = 0;
    bool any_received = false;
    std::map<u32, Chunk> ooo; // reliable and ordered: arrived ahead of rcv_nxt
    usize ooo_bytes = 0;
    std::set<u32> seen;       // unordered: arrived ahead of rcv_nxt (reliable), or lately (unreliable)
    u32 delivered = 0;        // unreliable ordered: the last sequence number delivered
    std::deque<Chunk> rcvq;
    usize rcv_bytes = 0;
    bool peer_fin = false;    // the peer's FIN taken in order: what is left in rcvq is the last
    bool fin_seen = false;    // reliable unordered: the peer's FIN came, at fin_seq, maybe before what precedes it
    u32 fin_seq = 0;
    bool ack_now = false;
    bool dropped = false; // gone from the library (a connection let go, or not accepted)
    u32 last_window = 0;
    s32 error = 0; // the last error (CELL_RUDP_OPTION_LAST_ERROR)
    bool writable_wanted = false, flush_wanted = false;
    u64 connect_deadline = 0, syn_next = 0, linger_deadline = 0, last_heard = 0, last_sent = 0;
    u32 syn_sends = 0;
    // Its statistics (CellRudpContextStatus).
    u32 lost = 0, sent_packets = 0, rcvd_packets = 0, retransmissions = 0;
    u64 sent_bytes = 0, rcvd_bytes = 0;

    usize queued() const { return queued_bytes; }
    Segment* first_unsent() { return sent < sndq.size() ? &sndq[sent] : nullptr; }
    // The oldest segment, off the queue (acknowledged, or lost without delivery guarantees).
    void pop_front()
    {
        const Segment& s = sndq.front();
        if (s.sends) {
            sent--;
            if (!s.sacked) flight--;
        }
        queued_bytes -= s.data.size();
        sndq.pop_front();
    }
    void clear_send()
    {
        sndq.clear();
        stream_out.clear();
        sent = queued_bytes = 0;
        flight = 0;
    }
    bool connected() const { return state == kStateEstablished || state == kStateCloseWait; }
    bool connecting() const { return state == kStateSynSent || state == kStateSynRcvd; }
};

// What the library knows of one socket: the contexts bound at each vport, and the connections on it (the bound
// contexts that have a peer, a listener's connections, those lingering after cellRudpTerminate).
struct Binding {
    u8 mux = kMuxNormal;
    bool p2p = false;
    bool closed = false; // the game closed the socket under its contexts: no longer read
    std::map<u16, std::shared_ptr<Context>> ports;
    std::vector<std::shared_ptr<Context>> conns;
};

struct Poll {
    u32 size = 0;
    std::vector<std::pair<s32, u16>> entries; // context ID, events asked for
    u64 cancels = 0;
};

struct Lib {
    std::mutex m; // everything below but the I/O buffers' users (rx_m) and the filter (filter_m)
    bool initialized = false;
    u32 app_malloc = 0, app_free = 0;
    u32 handler = 0, handler_arg = 0; // CellRudpEventHandler
    u16 mss = kDefaultMss;
    std::map<s32, std::shared_ptr<Context>> contexts;
    std::map<s32, Binding> sockets;
    std::map<s32, Poll> polls;
    std::vector<std::shared_ptr<Context>> lingering;
    s32 next_context = 1, next_poll = 1; // never reused within a process (a wait across End and Init sees that)
    bool io = false;
    std::atomic<bool> io_stop{false};
    u32 io_thread = 0, io_block = 0;
    std::mutex rx_m; // cellRudpProcessEvents' receive buffer, one caller at a time
    u32 rx_block = 0;
    lv2::WaitQueue waiters;
    std::array<u64, kStatCount> stats{};
    u64 records = 0; // bytes of context records held
    std::mt19937 rng{std::random_device{}()};
    std::mutex filter_m;
    std::atomic<bool> has_filter{false};
    rudp::SendFilter filter;
    bool raw_logged = false;
};

Lib& lib_of() { return HleProcess::current()->state<Lib>(); }

// The contexts the library holds: the game's, the connections listeners have not had accepted, those lingering.
u32 total_contexts(const Lib& lib)
{
    usize n = lib.contexts.size() + lib.lingering.size();
    for (const auto& [id, c] : lib.contexts) n += c->pending.size();
    return static_cast<u32>(n);
}

// What a call leaves to do once the library's lock is released: packets to send, records to give back, the handlers'
// events, sockets released. The handlers and allocator are the library's as they were.
struct Outgoing {
    rudp::Packet packet;
    bool raw = false;
};
struct Event {
    u32 handler = 0, arg = 0;
    s32 ctx = 0, event = 0, error = 0;
};
struct Record {
    u32 addr = 0, app_free = 0;
    bool own = false;
};
struct Work {
    std::vector<Outgoing> out;
    std::vector<Record> records;
    std::vector<Event> events;
    std::vector<s32> released;
    u32 handler = 0, handler_arg = 0;
};

Work work_of(const Lib& lib)
{
    Work w;
    w.handler = lib.handler;
    w.handler_arg = lib.handler_arg;
    return w;
}

// ------------------------------------------------------------------------------------------------- guest memory

// Committed guest memory, page by page (guest_readable checks up to one host page).
bool guest_range(lv2::PpuThread& t, u32 addr, u64 size)
{
    if (!addr || u64{addr} + size > 0x100000000ull) return false;
    for (u64 at = 0; at < size; at += 0x1000)
        if (!guest_readable(t, static_cast<u32>(addr + at), static_cast<u32>(std::min<u64>(size - at, 0x1000))))
            return false;
    return true;
}

// Guest memory for one packet on the calling thread: its stack for a small one, else a block of the main area.
class Scratch {
public:
    Scratch(PPUContext& ctx, u32 size)
    {
        if (size <= kStackScratch) {
            var_.emplace(ctx, guest_base(ctx), size);
            addr = var_->addr();
        } else {
            addr = HleProcess::current()->main_memory().alloc(size, 16);
            heap_ = addr != 0;
        }
    }
    ~Scratch()
    {
        if (heap_) HleProcess::current()->main_memory().free(addr);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    u32 addr = 0;

private:
    std::optional<StackVar> var_;
    bool heap_ = false;
};

// A guest function the game gave (a handler, its allocator), called when its descriptor and entry are guest memory;
// else logged and skipped (D-042): nullopt.
std::optional<u64> call_game(PPUContext& ctx, u32 opd, std::initializer_list<u64> args, std::string_view what)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!opd || !guest_range(t, opd, 8) || !guest_range(t, ps_read32(t.kernel.base(), opd), 4)) {
        PS_LOG_ERROR(kChan, "{} {:#x} is not a function in guest memory: not called", what, opd);
        return std::nullopt;
    }
    return call_guest(ctx, opd, args);
}

u64 sext(s32 v) { return static_cast<u64>(static_cast<s64>(v)); }

// A context's record: from the game's app_malloc, or the main area for the library's own allocator. 0 when none came.
Record alloc_record(PPUContext& ctx, u32 app_malloc, u32 app_free)
{
    lv2::PpuThread& t = thread_of(ctx);
    Record r;
    if (app_malloc) {
        const std::optional<u64> v = call_game(ctx, app_malloc, {kRecordSize}, "app_malloc");
        r.addr = v ? static_cast<u32>(*v) : 0;
        if (r.addr && !guest_range(t, r.addr, kRecordSize)) {
            PS_LOG_ERROR(kChan, "app_malloc({}) gave {:#x}, not guest memory", kRecordSize, r.addr);
            r.addr = 0;
        }
        r.app_free = app_free;
    } else {
        r.addr = HleProcess::current()->main_memory().alloc(kRecordSize, 16);
        r.own = true;
    }
    if (r.addr) std::memset(t.kernel.base() + r.addr, 0, kRecordSize);
    return r;
}

void free_record(PPUContext& ctx, const Record& r)
{
    if (!r.addr) return;
    if (r.own) HleProcess::current()->main_memory().free(r.addr);
    else if (r.app_free) call_game(ctx, r.app_free, {r.addr}, "app_free");
}

// ------------------------------------------------------------------------------------------------- waiting

// Wakes every thread waiting in the library: a context's state, data or a poll set changed.
void notify(Lib& lib)
{
    for (lv2::Waiter* w : lib.waiters.take_all(lv2::SYS_SYNC_FIFO)) lv2::wake(*w, lv2::CELL_OK);
}

// Parks the calling thread until notify or `deadline` (guest microseconds; 0: none). `lk` holds lib.m on entry and on
// return. False when the deadline passed. The process ending unwinds the thread (ThreadExit), as it does a blocked
// lv2 call; the wait shows in the thread report as a network one.
bool wait_change(Lib& lib, lv2::PpuThread& t, std::unique_lock<std::mutex>& lk, u64 deadline)
{
    u64 timeout = 0;
    if (deadline) {
        const u64 now = now_us();
        if (now >= deadline) return false;
        timeout = deadline - now;
    }
    lv2::Waiter& w = t.waiter;
    lv2::prepare_wait(w);
    lib.waiters.push(&w);
    const lv2::WaitStatus status = lv2::park(w, lk, timeout, {lv2::WaitKind::Net, 0}, [&](bool) {
        return lib.waiters.remove(&w) ? lv2::CancelResult::Removed : lv2::CancelResult::Woken;
    });
    if (status == lv2::WaitStatus::Aborted) throw lv2::ThreadExit{};
    lk.lock();
    return status == lv2::WaitStatus::Woken;
}

// The deadline of a wait of `timeout` microseconds from now (0, or kForever and more: none).
u64 deadline_after(u64 timeout) { return timeout && timeout < kForever ? now_us() + timeout : 0; }

// ------------------------------------------------------------------------------------------------- statistics

void count_sent(Lib& lib, Type type, usize bytes)
{
    lib.stats[kSentUdpPackets]++;
    lib.stats[kSentUdpBytes] += bytes;
    switch (type) {
    case Type::Syn:
    case Type::SynAck: lib.stats[kSentSynPackets]++; break;
    case Type::Data: lib.stats[kSentUsrPackets]++; break;
    case Type::Probe: lib.stats[kSentPrbPackets]++; break;
    case Type::Rst: lib.stats[kSentRstPackets]++; break;
    default: break;
    }
}

void count_received(Lib& lib, Type type)
{
    switch (type) {
    case Type::Syn:
    case Type::SynAck: lib.stats[kRcvdSynPackets]++; break;
    case Type::Data: lib.stats[kRcvdUsrPackets]++; break;
    case Type::Probe: lib.stats[kRcvdPrbPackets]++; break;
    case Type::Rst: lib.stats[kRcvdRstPackets]++; break;
    default: break;
    }
}

// The memory the library holds: the contexts' records and what they buffer.
void note_memory(Lib& lib)
{
    u64 bytes = lib.records;
    const auto add = [&](const Context& c) { bytes += c.queued() + c.rcv_bytes + c.ooo_bytes; };
    for (const auto& [id, c] : lib.contexts) {
        add(*c);
        for (const auto& k : c->pending) add(*k);
    }
    for (const auto& c : lib.lingering) add(*c);
    lib.stats[kMemCurrent] = bytes;
    lib.stats[kMemPeak] = std::max(lib.stats[kMemPeak], bytes);
}

void count_failure(Lib& lib, s32 err)
{
    lib.stats[kFailedConnections]++;
    switch (static_cast<u32>(err)) {
    case CELL_RUDP_ERROR_CONN_RESET: lib.stats[kFailedReset]++; break;
    case CELL_RUDP_ERROR_CONN_REFUSED: lib.stats[kFailedRefused]++; break;
    case CELL_RUDP_ERROR_CONN_TIMEOUT: lib.stats[kFailedTimeout]++; break;
    case CELL_RUDP_ERROR_CONN_VERSION_MISMATCH: lib.stats[kFailedVersion]++; break;
    case CELL_RUDP_ERROR_CONN_TRANSPORT_TYPE_MISMATCH: lib.stats[kFailedTransport]++; break;
    case CELL_RUDP_ERROR_QUALITY_LEVEL_MISMATCH: lib.stats[kFailedQuality]++; break;
    default: break;
    }
}

// ------------------------------------------------------------------------------------------------- packets out

u32 window_of(const Context& c)
{
    return c.opt.rcvbuf > c.rcv_bytes ? c.opt.rcvbuf - static_cast<u32>(c.rcv_bytes) : 0;
}

u32 ack_of(const Context& c) { return c.p.reliable || !c.any_received ? c.rcv_nxt : c.highest + 1; }

u32 sack_of(const Context& c)
{
    if (!c.p.reliable) return 0;
    u32 bits = 0;
    for (u32 i = 0; i < 32; i++) {
        const u32 seq = c.rcv_nxt + 1 + i;
        if (c.ooo.contains(seq) || c.seen.contains(seq)) bits |= 1u << i;
    }
    return bits;
}

void event(Work& w, const Context& c, s32 ev, s32 err = 0)
{
    if (c.id && c.handler) w.events.push_back({c.handler, c.arg, c.id, ev, err});
}

// A packet of connection `c`: its vports, its acknowledgement state and window, `payload` after the header. An
// activated context that has not heard its peer yet sends to its own vport (D-630).
void queue(Lib& lib, Context& c, Header h, const u8* payload, usize n, u64 now, Work& w)
{
    h.src = c.vport;
    h.dst = c.peer_vport ? c.peer_vport : c.vport;
    if (h.type != Type::Syn && h.type != Type::Rst) {
        h.ack = ack_of(c);
        h.sack = sack_of(c);
    }
    h.window = window_of(c);
    if (!h.stamp) h.stamp = static_cast<u32>(now);
    Outgoing o;
    o.packet.soc = c.soc;
    o.packet.to = c.peer.raw;
    o.packet.bytes.resize(kHeaderSize + n);
    rudp::encode(h, o.packet.bytes.data());
    if (n) std::memcpy(o.packet.bytes.data() + kHeaderSize, payload, n);
    o.raw = c.mux == kMuxRaw;
    count_sent(lib, h.type, o.packet.bytes.size());
    c.sent_packets++;
    c.sent_bytes += o.packet.bytes.size();
    c.last_sent = now;
    c.ack_now = false;
    c.last_window = h.window;
    w.out.push_back(std::move(o));
}

void send_control(Lib& lib, Context& c, Type type, u64 now, Work& w, u8 reason = 0)
{
    Header h;
    h.type = type;
    h.reason = reason;
    queue(lib, c, h, nullptr, 0, now, w);
}

// A SYN or SYN-ACK: this side's sequence number and its connection's parameters.
void send_syn(Lib& lib, Context& c, u64 now, Work& w)
{
    Header h;
    h.type = c.state == kStateSynRcvd ? Type::SynAck : Type::Syn;
    h.seq = c.iss;
    if (c.activated && h.type == Type::Syn) h.flags |= rudp::kFlagSoft;
    std::array<u8, kSynSize> p{};
    p[0] = c.p.stream;
    p[1] = c.p.reliable;
    p[2] = c.p.ordered;
    ps_st16(p.data() + 4, c.mss);
    ps_st32(p.data() + 8, c.opt.rcvbuf);
    queue(lib, c, h, p.data(), p.size(), now, w);
    c.syn_sends++;
    c.syn_next = now + std::min(kInitialRto << std::min<u32>(c.syn_sends - 1, 3), kMaxRto);
}

// A RST to whoever sent `h` from `from` on socket `soc`, for which no connection here was found.
void reply_rst(Lib& lib, const Binding& b, s32 soc, const Endpoint& from, const Header& h, u8 reason, Work& w)
{
    Header r;
    r.type = Type::Rst;
    r.reason = reason;
    r.src = h.dst;
    r.dst = h.src;
    r.stamp = static_cast<u32>(now_us());
    Outgoing o;
    o.packet.soc = soc;
    o.packet.to = from.raw;
    o.packet.bytes.resize(kHeaderSize);
    rudp::encode(r, o.packet.bytes.data());
    o.raw = b.mux == kMuxRaw;
    count_sent(lib, Type::Rst, kHeaderSize);
    w.out.push_back(std::move(o));
}

u32 in_flight(const Context& c) { return c.flight; }

u64 acked_offset(const Context& c) { return c.sndq.empty() ? c.snd_offset : c.sndq.front().offset; }

// Whether the peer's window takes `size` more bytes at `offset`; one segment past it when nothing is in flight, so a
// window smaller than a segment never stalls the connection.
bool window_allows(const Context& c, u64 offset, usize size)
{
    if (offset + size <= acked_offset(c) + c.peer_window) return true;
    return c.peer_window > 0 && in_flight(c) == 0;
}

void send_segment(Lib& lib, Context& c, Segment& s, u64 now, Work& w)
{
    Header h;
    h.type = s.type;
    h.seq = s.seq;
    h.flags = s.flags;
    h.retries = static_cast<u8>(std::min<u32>(s.sends, 255));
    if (!s.sends) { // the first unsent one (Context::first_unsent)
        s.first_sent = now;
        s.stamp = static_cast<u32>(now);
        c.sent++;
        c.flight++;
    } else {
        lib.stats[kRetransmittedPackets]++;
        c.retransmissions++;
    }
    h.stamp = s.stamp ? s.stamp : 1;
    s.sends++;
    s.last_sent = now;
    queue(lib, c, h, s.data.data(), s.data.size(), now, w);
}

// Sends what the windows let go: the next unsent segments in order, a stream's bytes cut into segments of the payload
// (a part segment held back while others are in flight, up to the aggregation timeout, unless NODELAY), the FIN after
// them.
void push(Lib& lib, Context& c, u64 now, Work& w)
{
    if (!c.connected()) return;
    for (;;) {
        Segment* next = c.first_unsent();
        if (!next && c.p.stream && !c.stream_out.empty()) {
            const usize n = std::min<usize>(c.payload, c.stream_out.size());
            const bool part = n < c.payload;
            if (part && !c.opt.nodelay && !c.flush_now && in_flight(c) && c.opt.aggregation_timeout) {
                if (!c.nagle_since) c.nagle_since = now;
                if (now < c.nagle_since + c.opt.aggregation_timeout) break;
            }
            if (in_flight(c) >= static_cast<u32>(c.cwnd) || !window_allows(c, c.snd_offset, n)) break;
            Segment s;
            s.seq = c.snd_nxt++;
            s.offset = c.snd_offset;
            s.data.assign(c.stream_out.begin(), c.stream_out.begin() + static_cast<std::ptrdiff_t>(n));
            c.stream_out.erase(c.stream_out.begin(), c.stream_out.begin() + static_cast<std::ptrdiff_t>(n));
            c.snd_offset += n;
            c.nagle_since = 0;
            c.sndq.push_back(std::move(s));
            next = &c.sndq.back();
        }
        if (!next && c.fin_wanted && !c.fin_queued && c.stream_out.empty()) {
            Segment s;
            s.type = Type::Fin;
            s.seq = c.snd_nxt++;
            s.offset = c.snd_offset;
            c.sndq.push_back(std::move(s));
            c.fin_queued = true;
            next = &c.sndq.back();
        }
        if (!next) break;
        if (in_flight(c) >= static_cast<u32>(c.cwnd) || !window_allows(c, next->offset, next->data.size())) break;
        send_segment(lib, c, *next, now, w);
    }
    c.flush_now = false;
    if (in_flight(c) && !c.rto_deadline) c.rto_deadline = now + c.rto;
}

// ------------------------------------------------------------------------------------------------- connections

void link(Lib& lib, const std::shared_ptr<Context>& c) { lib.sockets[c->soc].conns.push_back(c); }

void unlink(Lib& lib, const Context& c)
{
    const auto it = lib.sockets.find(c.soc);
    if (it == lib.sockets.end()) return;
    std::erase_if(it->second.conns, [&](const std::shared_ptr<Context>& x) { return x.get() == &c; });
}

// A socket nothing uses any more is the game's again (CELL_RUDP_EVENT_SOCKET_RELEASED).
void release_if_unused(Lib& lib, s32 soc, Work& w)
{
    const auto it = lib.sockets.find(soc);
    if (it == lib.sockets.end() || !it->second.ports.empty() || !it->second.conns.empty()) return;
    lib.sockets.erase(it);
    w.released.push_back(soc);
}

void drop_lingering(Lib& lib, Context& c, Work& w)
{
    c.dropped = true;
    unlink(lib, c);
    std::erase_if(lib.lingering, [&](const std::shared_ptr<Context>& x) { return x.get() == &c; });
    release_if_unused(lib, c.soc, w);
}

// A listener's connection gone before it was accepted.
void drop_pending(Lib& lib, Context& c, Work& w)
{
    c.dropped = true;
    unlink(lib, c);
    if (const auto parent = c.parent.lock()) {
        std::erase_if(parent->pending, [&](const std::shared_ptr<Context>& x) { return x.get() == &c; });
    }
    release_if_unused(lib, c.soc, w);
}

// The connection ends with `err`: the context is CLOSED, what it buffered is dropped, its handler hears ERROR and
// CLOSED. The game still terminates it.
void fail(Lib& lib, Context& c, s32 err, Work& w)
{
    if (c.lingering) {
        drop_lingering(lib, c, w);
        return;
    }
    if (c.connecting()) count_failure(lib, err);
    PS_LOG_INFO(kChan, "context {} (vport {}): the connection ends with {:#x}", c.id, c.vport, static_cast<u32>(err));
    c.state = kStateClosed;
    c.error = err;
    c.clear_send();
    c.ooo.clear();
    c.ooo_bytes = 0;
    c.seen.clear();
    c.rcvq.clear();
    c.rcv_bytes = 0;
    c.rto_deadline = c.persist_deadline = 0;
    if (!c.id && c.parent.lock()) {
        drop_pending(lib, c, w);
        return;
    }
    unlink(lib, c);
    event(w, c, kEventError, err);
    event(w, c, kEventClosed, err);
    notify(lib);
}

u8 reason_of(s32 err)
{
    switch (static_cast<u32>(err)) {
    case CELL_RUDP_ERROR_CONN_TRANSPORT_TYPE_MISMATCH: return rudp::kRstTransport;
    case CELL_RUDP_ERROR_QUALITY_LEVEL_MISMATCH: return rudp::kRstQuality;
    case CELL_RUDP_ERROR_CONN_VERSION_MISMATCH: return rudp::kRstVersion;
    default: return rudp::kRstReset;
    }
}

s32 error_of_reset(u8 reason, bool connecting)
{
    switch (reason) {
    case rudp::kRstRefused: return error(connecting ? CELL_RUDP_ERROR_CONN_REFUSED : CELL_RUDP_ERROR_CONN_RESET);
    case rudp::kRstVersion: return error(CELL_RUDP_ERROR_CONN_VERSION_MISMATCH);
    case rudp::kRstTransport: return error(CELL_RUDP_ERROR_CONN_TRANSPORT_TYPE_MISMATCH);
    case rudp::kRstQuality: return error(CELL_RUDP_ERROR_QUALITY_LEVEL_MISMATCH);
    default: return error(CELL_RUDP_ERROR_CONN_RESET);
    }
}

// Why a SYN's parameters do not fit what `p` carries (0 when they do); nullopt for a SYN too short to have them.
std::optional<s32> mismatch(const Params& p, const u8* payload, usize n)
{
    if (n < kSynSize) return std::nullopt;
    const bool stream = payload[0] != 0;
    if (stream != p.stream) return error(CELL_RUDP_ERROR_CONN_TRANSPORT_TYPE_MISMATCH);
    if (!stream && ((payload[1] != 0) != p.reliable || (payload[2] != 0) != p.ordered))
        return error(CELL_RUDP_ERROR_QUALITY_LEVEL_MISMATCH);
    return 0;
}

// The peer's parameters from its SYN or SYN-ACK: the smaller segment, its window.
void take_peer(Context& c, const u8* payload)
{
    const u16 peer_mss = ps_ld16(payload + 4);
    if (peer_mss >= kMinMss && peer_mss < c.mss) c.mss = peer_mss;
    c.payload = c.mss - kHeaderSize;
    c.peer_window = ps_ld32(payload + 8);
}

// The handshake's end: the connection carries data both ways.
void establish(Lib& lib, Context& c, u64 now, Work& w)
{
    c.state = kStateEstablished;
    c.snd_una = c.iss + 1;
    c.rto = kInitialRto;
    c.rto_deadline = 0;
    c.last_heard = now;
    lib.stats[kEstablishedConnections]++;
    const std::shared_ptr<Context> listener = c.id ? nullptr : c.parent.lock(); // not accepted yet
    const std::string who = c.id ? std::format("context {}", c.id)
                                 : std::format("a connection for listener {}", listener ? listener->id : 0);
    PS_LOG_INFO(kChan, "{} (vport {}): established with vport {}{}", who, c.vport, c.peer_vport,
        c.p.stream ? ", a stream" : "");
    event(w, c, kEventEstablished);
    if (listener) event(w, *listener, kEventReadable); // one to accept
    push(lib, c, now, w);
    notify(lib);
}

// A connection starting from this side (Initiate, Activate) or from a SYN a listener took: its sequence numbers and
// segment size; the SYN or SYN-ACK goes out at once.
void start(Lib& lib, Context& c, u64 now)
{
    c.p = params_of(c.opt);
    c.mss = lib.mss;
    c.payload = c.mss - kHeaderSize;
    c.iss = static_cast<u32>(lib.rng());
    c.snd_una = c.iss;
    c.snd_nxt = c.iss + 1;
    c.connect_deadline = c.opt.connection_timeout ? now + c.opt.connection_timeout : 0;
    c.syn_sends = 0;
    c.cwnd = kInitialCwnd;
    c.ssthresh = kInitialSsthresh;
    c.rto = kInitialRto;
    c.rack_xmit = 0;
    c.recovery = false;
    c.has_peer = true;
}

// A loss found (RACK or a retransmission timeout): the window shrinks once in a recovery episode.
void enter_recovery(Context& c, bool timeout)
{
    if (c.recovery && !timeout) return;
    c.ssthresh = std::max(2.0, std::min(c.cwnd, static_cast<double>(in_flight(c))) / 2);
    c.cwnd = timeout ? 2.0 : c.ssthresh;
    c.recovery = true;
    c.recover = c.snd_nxt;
}

// How long after its sending a segment the peer still lacks is lost once one sent after it has arrived: an RTT and a
// reordering window (a quarter of it, at least a millisecond).
u64 rack_wait(const Context& c) { return c.srtt + std::max<u64>(c.srtt / 4, 1000); }

// The segments the peer lacks that were sent before one it has, and long enough ago: sent again.
void detect_losses(Lib& lib, Context& c, u64 now, Work& w)
{
    if (!c.p.reliable || !c.rack_xmit) return;
    const u64 wait = rack_wait(c);
    for (usize i = 0; i < c.sent; i++) {
        Segment& s = c.sndq[i];
        if (s.sacked || s.last_sent >= c.rack_xmit || now < s.last_sent + wait) continue;
        enter_recovery(c, false);
        c.lost++;
        lib.stats[kLostPackets]++;
        send_segment(lib, c, s, now, w);
    }
}

// An RTT sample (Jacobson and Karels, as TCP's), from a segment acknowledged after one sending (Karn).
void sample_rtt(Context& c, u64 rtt)
{
    if (!c.rtt_valid) {
        c.srtt = rtt;
        c.rttvar = rtt / 2;
        c.rtt_valid = true;
    } else {
        const u64 d = c.srtt > rtt ? c.srtt - rtt : rtt - c.srtt;
        c.rttvar = (3 * c.rttvar + d) / 4;
        c.srtt = (7 * c.srtt + rtt) / 8;
    }
    c.rto = std::clamp(c.srtt + std::max<u64>(4 * c.rttvar, 1000), kMinRto, kMaxRto);
}

// What the peer acknowledged: segments off the queue, the RTT, the congestion window, the selective
// acknowledgements, the losses they show (detect_losses); the peer's window.
void on_ack(Lib& lib, Context& c, const Header& h, u64 now, Work& w)
{
    const usize before = c.queued();
    u32 newly = 0;
    if (seq_gt(h.ack, c.snd_una) && !seq_gt(h.ack, c.snd_nxt)) {
        while (!c.sndq.empty() && seq_lt(c.sndq.front().seq, h.ack) && c.sndq.front().sends) {
            const Segment& s = c.sndq.front();
            if (s.sends == 1 && s.type == Type::Data && c.p.reliable) sample_rtt(c, now - s.first_sent);
            c.rack_xmit = std::max(c.rack_xmit, s.last_sent);
            c.pop_front();
            newly++;
        }
        c.snd_una = h.ack;
        if (c.recovery && !seq_lt(c.snd_una, c.recover)) c.recovery = false;
        if (c.p.reliable) {
            if (!c.rtt_valid) c.rto = kInitialRto;
            else c.rto = std::clamp(c.srtt + std::max<u64>(4 * c.rttvar, 1000), kMinRto, kMaxRto);
        }
        c.rto_deadline = in_flight(c) ? now + c.rto : 0;
        if (!c.recovery)
            for (u32 i = 0; i < newly; i++) c.cwnd += c.cwnd < c.ssthresh ? 1.0 : 1.0 / c.cwnd;
        c.cwnd = std::min(c.cwnd, kMaxCwnd);
    }
    if (c.p.reliable && h.sack) {
        for (usize i = 0; i < c.sent; i++) {
            Segment& s = c.sndq[i];
            const u32 d = s.seq - h.ack - 1;
            if (d < 32 && (h.sack >> d) & 1 && !s.sacked) {
                s.sacked = true;
                c.flight--;
                c.rack_xmit = std::max(c.rack_xmit, s.last_sent);
            }
        }
    }
    detect_losses(lib, c, now, w);
    c.peer_window = h.window;
    if (c.peer_window) c.persist_deadline = 0;
    const usize after = c.queued();
    if (after < before) {
        if (c.writable_wanted && after < c.opt.sndbuf) {
            c.writable_wanted = false;
            event(w, c, kEventWritable);
        }
        if (!after && c.flush_wanted) {
            c.flush_wanted = false;
            event(w, c, kEventFlushed);
        }
        notify(lib);
    }
}

void deliver(Lib& lib, Context& c, Chunk&& chunk, Work& w)
{
    if (chunk.fin) c.peer_fin = true;
    if (c.lingering) return; // nobody reads a terminated context: its data acknowledged and dropped
    if (chunk.fin) {
        if (c.state == kStateEstablished) c.state = kStateCloseWait;
        PS_LOG_INFO(kChan, "context {} (vport {}): the peer ended the connection", c.id, c.vport);
        event(w, c, kEventClosed, 0);
        notify(lib);
        return;
    }
    const bool was_empty = c.rcvq.empty();
    const u32 level = quality_of(c.p);
    lib.stats[kRcvdUserBytes] += chunk.data.size();
    lib.stats[kRcvdUserPackets]++;
    lib.stats[kQualityLevels + 4 * level + 1] += chunk.data.size();
    lib.stats[kQualityLevels + 4 * level + 3]++;
    if (chunk.flags & rudp::kFlagLatency) lib.stats[kRcvdLatencyCriticalPackets]++;
    c.rcv_bytes += chunk.data.size();
    c.rcvq.push_back(std::move(chunk));
    if (was_empty) event(w, c, kEventReadable);
    notify(lib);
}

// A DATA or FIN segment into the receive side, by the connection's delivery and order guarantees.
void on_segment(Lib& lib, Context& c, const Header& h, const u8* payload, usize n, Work& w)
{
    Chunk chunk;
    chunk.data.assign(payload, payload + n);
    chunk.seq = h.seq;
    chunk.stamp = h.stamp;
    chunk.flags = h.flags;
    chunk.retries = h.retries;
    chunk.fin = h.type == Type::Fin;
    c.ack_now = true;
    if (c.peer_fin) return; // past the peer's end
    const usize slack = 2 * std::max<usize>(c.payload, 1);
    if (c.p.reliable) {
        if (seq_lt(h.seq, c.rcv_nxt) || c.ooo.contains(h.seq) || c.seen.contains(h.seq)) return; // a duplicate
        if (c.p.ordered) {
            if (h.seq != c.rcv_nxt) {
                if (c.rcv_bytes + c.ooo_bytes + n > c.opt.rcvbuf + slack || h.seq - c.rcv_nxt > 4096) return;
                lib.stats[kReorderedPackets]++;
                c.ooo_bytes += n;
                c.ooo.emplace(h.seq, std::move(chunk));
                return;
            }
            if (c.rcv_bytes + n > c.opt.rcvbuf + slack) return; // past the window: the peer sends it again
            c.rcv_nxt++;
            deliver(lib, c, std::move(chunk), w);
            for (auto it = c.ooo.find(c.rcv_nxt); it != c.ooo.end(); it = c.ooo.find(c.rcv_nxt)) {
                c.ooo_bytes -= it->second.data.size();
                Chunk next = std::move(it->second);
                c.ooo.erase(it);
                c.rcv_nxt++;
                deliver(lib, c, std::move(next), w);
            }
            return;
        }
        // Reliable, unordered: each message as it comes, its sequence number kept until the ones before it came; the
        // peer's end once everything before it came.
        if (c.rcv_bytes + n > c.opt.rcvbuf + slack || h.seq - c.rcv_nxt > 4096) return;
        if (h.seq != c.rcv_nxt) {
            lib.stats[kReorderedPackets]++;
            c.seen.insert(h.seq);
        } else {
            c.rcv_nxt++;
            while (c.seen.erase(c.rcv_nxt)) c.rcv_nxt++;
        }
        if (chunk.fin) {
            c.fin_seen = true;
            c.fin_seq = h.seq;
        } else {
            deliver(lib, c, std::move(chunk), w);
        }
        if (c.fin_seen && c.rcv_nxt == c.fin_seq + 1) {
            Chunk end;
            end.fin = true;
            deliver(lib, c, std::move(end), w);
        }
        return;
    }
    // Unreliable: no retransmission; the highest sequence number seen is acknowledged.
    if (!c.any_received || seq_gt(h.seq, c.highest)) c.highest = h.seq;
    const bool first = !c.any_received;
    c.any_received = true;
    if (chunk.fin) {
        deliver(lib, c, std::move(chunk), w);
        return;
    }
    if (c.rcv_bytes + n > c.opt.rcvbuf + slack) return; // no room: lost
    if (c.p.ordered) {
        if (!first && !seq_gt(h.seq, c.delivered)) return; // late: an older message than one delivered
        c.delivered = h.seq;
    } else {
        if (c.seen.contains(h.seq)) return;
        c.seen.insert(h.seq);
        while (c.seen.size() > 1024) c.seen.erase(c.seen.begin());
        if (!first && seq_lt(h.seq, c.highest)) lib.stats[kReorderedPackets]++;
    }
    deliver(lib, c, std::move(chunk), w);
}

// A SYN for a listener's vport from a peer it has no connection with: a connection of its own, SYN-ACK sent, which the
// game takes with cellRudpAccept once the handshake ends.
void on_listen_syn(Lib& lib, Binding& b, const std::shared_ptr<Context>& l, const Endpoint& from, const Header& h,
    const u8* payload, usize n, u64 now, Work& w)
{
    const std::optional<s32> bad = mismatch(params_of(l->opt), payload, n);
    if (!bad) return;
    if (*bad) {
        reply_rst(lib, b, l->soc, from, h, reason_of(*bad), w);
        count_failure(lib, *bad);
        return;
    }
    // Full (the backlog, the library's contexts): the peer sends its SYN again.
    if (l->pending.size() >= l->backlog || total_contexts(lib) >= kMaxContexts) return;
    auto c = std::make_shared<Context>();
    c->handler = l->handler;
    c->arg = l->arg;
    c->opt = l->opt;
    c->bound = true;
    c->soc = l->soc;
    c->vport = l->vport;
    c->mux = l->mux;
    c->parent = l;
    c->peer = from;
    c->peer_vport = h.src;
    start(lib, *c, now);
    c->irs = h.seq;
    c->rcv_nxt = h.seq + 1;
    take_peer(*c, payload);
    c->state = kStateSynRcvd;
    c->last_heard = now;
    b.conns.push_back(c);
    l->pending.push_back(c);
    send_syn(lib, *c, now, w);
}

// A SYN on a connection: a simultaneous open (Initiate or Activate meeting the peer's SYN), or a SYN again.
void on_syn(Lib& lib, Binding& b, Context& c, const Endpoint& from, const Header& h, const u8* payload, usize n,
    u64 now, Work& w)
{
    if (c.state == kStateSynSent) {
        const std::optional<s32> bad = mismatch(c.p, payload, n);
        if (!bad) return;
        if (*bad) {
            reply_rst(lib, b, c.soc, from, h, reason_of(*bad), w);
            fail(lib, c, *bad, w);
            return;
        }
        if (!c.peer_vport) c.peer_vport = h.src;
        c.irs = h.seq;
        c.rcv_nxt = h.seq + 1;
        take_peer(c, payload);
        c.state = kStateSynRcvd;
        c.syn_sends = 0;
        send_syn(lib, c, now, w);
        return;
    }
    if (h.seq != c.irs) { // the peer started again
        reply_rst(lib, b, c.soc, from, h, rudp::kRstReset, w);
        fail(lib, c, error(CELL_RUDP_ERROR_CONN_RESET), w);
        return;
    }
    if (c.state == kStateSynRcvd) send_syn(lib, c, now, w);
    else c.ack_now = true; // our SYN-ACK or ACK was lost
}

void on_synack(Lib& lib, Binding& b, Context& c, const Endpoint& from, const Header& h, const u8* payload, usize n,
    u64 now, Work& w)
{
    if (c.state == kStateSynSent) {
        if (h.ack != c.iss + 1) return;
        const std::optional<s32> bad = mismatch(c.p, payload, n);
        if (!bad) return;
        if (*bad) {
            reply_rst(lib, b, c.soc, from, h, reason_of(*bad), w);
            fail(lib, c, *bad, w);
            return;
        }
        if (!c.peer_vport) c.peer_vport = h.src;
        c.irs = h.seq;
        c.rcv_nxt = h.seq + 1;
        take_peer(c, payload);
        establish(lib, c, now, w);
        c.ack_now = true;
        return;
    }
    if (c.state == kStateSynRcvd) {
        if (h.ack != c.iss + 1 || h.seq != c.irs) return;
        if (n >= kSynSize) take_peer(c, payload);
        establish(lib, c, now, w);
        c.ack_now = true;
        return;
    }
    if (h.seq == c.irs) c.ack_now = true; // our ACK was lost
}

// The connection on `b` a packet from `from` with header `h` belongs to.
std::shared_ptr<Context> find_conn(const Binding& b, const Endpoint& from, const Header& h)
{
    for (const auto& c : b.conns) {
        if (c->vport != h.dst || !same_peer(c->peer, from, b.p2p)) continue;
        if (c->peer_vport == h.src) return c;
        if (!c->peer_vport && c->activated && c->state == kStateSynSent) return c;
    }
    return nullptr;
}

// One packet that came on socket `soc` from `from`. False when it is not an RUDP packet (the magic, the size, the
// type).
bool receive(Lib& lib, s32 soc, const Endpoint& from, const u8* bytes, usize size, u64 now, Work& w)
{
    const auto bit = lib.sockets.find(soc);
    if (bit == lib.sockets.end()) return true; // no context uses it any more
    Binding& b = bit->second;
    const std::optional<Header> hh = rudp::decode(bytes, size);
    if (!hh) return false;
    const Header& h = *hh;
    if (h.version == rudp::kVersion && (h.type < Type::Syn || h.type > Type::Rst)) return false;
    lib.stats[kRcvdUdpPackets]++;
    lib.stats[kRcvdUdpBytes] += size;
    if (h.version != rudp::kVersion) {
        if (h.type != Type::Rst) reply_rst(lib, b, soc, from, h, rudp::kRstVersion, w);
        return true;
    }
    count_received(lib, h.type);
    const u8* payload = bytes + kHeaderSize;
    const usize n = size - kHeaderSize;
    std::shared_ptr<Context> c = find_conn(b, from, h);
    if (c && c->lingering && h.type == Type::Syn && h.seq != c->irs) { // a new connection over one let go
        drop_lingering(lib, *c, w);
        if (!lib.sockets.contains(soc)) return true;
        c = find_conn(b, from, h);
    }
    if (!c) {
        if (h.type == Type::Syn) {
            const auto port = b.ports.find(h.dst);
            if (port != b.ports.end() && port->second->listening) {
                on_listen_syn(lib, b, port->second, from, h, payload, n, now, w);
                return true;
            }
            // A context bound there that has not started yet (two Initiates, or an Activate, one a little later than
            // the other) or one with another peer: dropped, the SYN comes again. An Activate's SYN where nothing waits.
            if (port != b.ports.end() || (h.flags & rudp::kFlagSoft)) return true;
        }
        if (h.type != Type::Rst)
            reply_rst(lib, b, soc, from, h, h.type == Type::Syn ? rudp::kRstRefused : rudp::kRstReset, w);
        return true;
    }
    Context& x = *c;
    x.rcvd_packets++;
    x.rcvd_bytes += size;
    x.last_heard = now;
    switch (h.type) {
    case Type::Rst: fail(lib, x, error_of_reset(h.reason, x.connecting()), w); return true;
    case Type::Syn: on_syn(lib, b, x, from, h, payload, n, now, w); break;
    case Type::SynAck: on_synack(lib, b, x, from, h, payload, n, now, w); break;
    case Type::Ack:
    case Type::Probe:
    case Type::Data:
    case Type::Fin:
        if (x.state == kStateSynRcvd && h.ack == x.iss + 1) establish(lib, x, now, w);
        if (!x.connected()) break;
        on_ack(lib, x, h, now, w);
        if (h.type == Type::Probe) x.ack_now = true;
        if (h.type == Type::Data || h.type == Type::Fin) on_segment(lib, x, h, payload, n, w);
        break;
    }
    if (x.dropped || x.state == kStateClosed) return true;
    push(lib, x, now, w);
    if (x.ack_now && x.has_peer) send_control(lib, x, Type::Ack, now, w);
    if (x.lingering && x.sndq.empty() && x.fin_queued && x.peer_fin) drop_lingering(lib, x, w); // both ends done
    note_memory(lib);
    return true;
}

// ------------------------------------------------------------------------------------------------- timers

// What is due on connection `c` at `now`: the handshake's next SYN or its timeout, a retransmission, a held segment,
// a window probe, keep-alive, the end of a terminated connection's time to close.
void on_timers(Lib& lib, Context& c, u64 now, Work& w)
{
    if (c.connecting()) {
        if (c.connect_deadline && now >= c.connect_deadline) {
            fail(lib, c, error(CELL_RUDP_ERROR_CONN_TIMEOUT), w);
            return;
        }
        if (now >= c.syn_next) send_syn(lib, c, now, w);
        return;
    }
    if (!c.connected()) return;
    if (c.lingering && now >= c.linger_deadline) {
        send_control(lib, c, Type::Rst, now, w, rudp::kRstReset);
        drop_lingering(lib, c, w);
        return;
    }
    if (c.opt.keep_alive_timeout && now >= c.last_heard + c.opt.keep_alive_timeout) {
        fail(lib, c, error(CELL_RUDP_ERROR_KEEP_ALIVE_FAILURE), w);
        return;
    }
    if (!c.p.reliable) { // what was sent and is not acknowledged within the timeout is lost; a FIN is sent again
        while (!c.sndq.empty() && c.sndq.front().sends && c.sndq.front().type == Type::Data &&
               now >= c.sndq.front().last_sent + c.rto) {
            c.lost++;
            lib.stats[kLostPackets]++;
            c.pop_front();
            notify(lib);
        }
    }
    detect_losses(lib, c, now, w);
    if (c.rto_deadline && now >= c.rto_deadline) {
        Segment* oldest = nullptr;
        for (usize i = 0; i < c.sent && !oldest; i++)
            if (!c.sndq[i].sacked && (c.p.reliable || c.sndq[i].type == Type::Fin)) oldest = &c.sndq[i];
        if (oldest) {
            c.lost++;
            lib.stats[kLostPackets]++;
            enter_recovery(c, true);
            send_segment(lib, c, *oldest, now, w);
            c.rto = std::min(c.rto * 2, kMaxRto);
            c.rto_deadline = now + c.rto;
        } else {
            c.rto_deadline = in_flight(c) ? now + c.rto : 0;
        }
    }
    push(lib, c, now, w); // a held part segment, a window that opened
    const bool waiting = !c.stream_out.empty() || c.sent < c.sndq.size();
    if (waiting && !in_flight(c) && !c.peer_window) { // the peer's window is shut: ask it now and then
        if (!c.persist_deadline) c.persist_deadline = now + c.rto;
        else if (now >= c.persist_deadline) {
            send_control(lib, c, Type::Probe, now, w);
            c.persist_deadline = now + std::min(c.rto * 2, kMaxRto);
        }
    }
    if (c.opt.keep_alive_interval && now >= c.last_sent + c.opt.keep_alive_interval)
        send_control(lib, c, Type::Probe, now, w);
}

// The earliest time something is due on `c` (0: nothing).
u64 next_due(const Context& c)
{
    u64 due = 0;
    const auto at = [&](u64 t) {
        if (t && (!due || t < due)) due = t;
    };
    if (c.connecting()) {
        at(c.connect_deadline);
        at(c.syn_next);
        return due;
    }
    if (!c.connected()) return 0;
    if (c.lingering) at(c.linger_deadline);
    if (c.opt.keep_alive_timeout) at(c.last_heard + c.opt.keep_alive_timeout);
    if (c.opt.keep_alive_interval) at(c.last_sent + c.opt.keep_alive_interval);
    at(c.rto_deadline);
    at(c.persist_deadline);
    if (c.p.reliable && c.rack_xmit)
        for (usize i = 0; i < c.sent; i++)
            if (!c.sndq[i].sacked && c.sndq[i].last_sent < c.rack_xmit) at(c.sndq[i].last_sent + rack_wait(c));
    if (c.nagle_since) at(c.nagle_since + c.opt.aggregation_timeout);
    if (!c.p.reliable && !c.sndq.empty() && c.sndq.front().sends) at(c.sndq.front().last_sent + c.rto);
    return due;
}

template <class F>
void each_connection(Lib& lib, F&& f)
{
    std::vector<std::shared_ptr<Context>> all;
    for (const auto& [id, c] : lib.contexts) {
        all.push_back(c);
        for (const auto& k : c->pending) all.push_back(k);
    }
    for (const auto& c : lib.lingering) all.push_back(c);
    for (const auto& c : all) f(*c);
}

void run_timers(Lib& lib, u64 now, Work& w)
{
    each_connection(lib, [&](Context& c) { on_timers(lib, c, now, w); });
    note_memory(lib);
}

u64 next_deadline(Lib& lib)
{
    u64 due = 0;
    each_connection(lib, [&](Context& c) {
        const u64 t = next_due(c);
        if (t && (!due || t < due)) due = t;
    });
    return due;
}

// ------------------------------------------------------------------------------------------------- doing the work

void send_packet(PPUContext& ctx, Lib& lib, const Work& w, const rudp::Packet& p, bool raw)
{
    u8* base = guest_base(ctx);
    const u32 len = static_cast<u32>(p.bytes.size());
    Scratch s(ctx, kSockaddrSize + len);
    if (!s.addr) {
        PS_LOG_ERROR(kChan, "no guest memory to send a packet of {} bytes from", len);
        return;
    }
    std::memcpy(base + s.addr, p.to.data(), kSockaddrSize);
    std::memcpy(base + s.addr + kSockaddrSize, p.bytes.data(), len);
    if (raw) {
        if (!w.handler) {
            std::lock_guard lock(lib.m);
            if (!lib.raw_logged) PS_LOG_WARN(kChan, "a RAW socket's packet with no event handler to send it: lost");
            lib.raw_logged = true;
            return;
        }
        call_game(ctx, w.handler,
            {static_cast<u64>(kEventSend), sext(p.soc), s.addr + kSockaddrSize, len, s.addr, kSockaddrSize,
                w.handler_arg},
            "the event handler");
        return;
    }
    const s32 r = static_cast<s32>(
        lv2_syscall(ctx, kSysSendto, {sext(p.soc), s.addr + kSockaddrSize, len, 0, s.addr, kSockaddrSize}));
    if (r < 0) PS_LOG_DEBUG(kChan, "sendto(s {}, {} bytes): errno {}", p.soc, len, -r);
}

// Sends the packets, gives the records back, calls the handlers: with no lock of the library held.
void finish(PPUContext& ctx, Lib& lib, Work& w)
{
    for (Outgoing& o : w.out) {
        if (!lib.has_filter.load(std::memory_order_acquire)) {
            send_packet(ctx, lib, w, o.packet, o.raw);
            continue;
        }
        rudp::SendFilter filter;
        {
            std::lock_guard lock(lib.filter_m);
            filter = lib.filter;
        }
        std::vector<rudp::Packet> now;
        if (filter) filter(std::move(o.packet), now);
        else now.push_back(std::move(o.packet));
        for (const rudp::Packet& p : now) send_packet(ctx, lib, w, p, o.raw);
    }
    for (const Record& r : w.records) free_record(ctx, r);
    for (const Event& e : w.events)
        call_game(ctx, e.handler, {sext(e.ctx), sext(e.event), sext(e.error), e.arg}, "the context handler");
    if (w.handler)
        for (const s32 soc : w.released)
            call_game(ctx, w.handler, {static_cast<u64>(kEventSocketReleased), sext(soc), 0, 0, 0, 0, w.handler_arg},
                "the event handler");
    w = Work{};
}

// ------------------------------------------------------------------------------------------------- the library's I/O

// The NORMAL-mode sockets the library reads.
std::vector<s32> normal_sockets(Lib& lib)
{
    std::vector<s32> s;
    for (const auto& [soc, b] : lib.sockets)
        if (b.mux == kMuxNormal && !b.closed) s.push_back(soc);
    return s;
}

// Every datagram waiting on `sockets`, received into `buf` (kRxSize bytes, then the source address and its length)
// and processed. The number taken.
u32 drain(PPUContext& ctx, Lib& lib, u32 buf, const std::vector<s32>& sockets)
{
    u8* base = guest_base(ctx);
    const u32 from = buf + kRxSize, fromlen = from + kSockaddrSize;
    u32 taken = 0;
    for (const s32 soc : sockets) {
        for (u32 k = 0; k < kDrainLimit; k++) {
            ps_write32(base, fromlen, kSockaddrSize);
            const s32 n = static_cast<s32>(
                lv2_syscall(ctx, kSysRecvfrom, {sext(soc), buf, kRxSize, kMsgDontwaitNet, from, fromlen}));
            if (n <= 0) break;
            taken++;
            const Endpoint e = endpoint_of(base + from);
            std::vector<u8> bytes(base + buf, base + buf + n);
            Work w;
            {
                std::lock_guard lock(lib.m);
                if (!lib.initialized) return taken;
                w = work_of(lib);
                if (!receive(lib, soc, e, bytes.data(), bytes.size(), now_us(), w))
                    PS_LOG_DEBUG(kChan, "socket {}: a datagram of {} bytes that is not RUDP, dropped", soc, n);
            }
            finish(ctx, lib, w);
        }
    }
    return taken;
}

void timers(PPUContext& ctx, Lib& lib)
{
    Work w;
    {
        std::lock_guard lock(lib.m);
        if (!lib.initialized) return;
        w = work_of(lib);
        run_timers(lib, now_us(), w);
    }
    finish(ctx, lib, w);
}

// Whether the calling thread is in the library's I/O (a handler it called calling cellRudpProcessEvents).
thread_local bool t_pumping = false;

// The library's own I/O (the internal thread's, cellRudpProcessEvents'): its NORMAL-mode sockets read, the timers that
// are due run; with `wait`, then up to that long (microseconds) for a datagram or the next timer, and once more.
// `buf`: kIoBlockSize bytes of guest memory.
void pump(PPUContext& ctx, Lib& lib, u32 buf, u64 wait)
{
    lv2::PpuThread& t = thread_of(ctx);
    t_pumping = true;
    const struct Done {
        ~Done() { t_pumping = false; }
    } done;
    std::vector<s32> sockets;
    {
        std::lock_guard lock(lib.m);
        if (!lib.initialized) return;
        sockets = normal_sockets(lib);
    }
    const u32 got = drain(ctx, lib, buf, sockets);
    timers(ctx, lib);
    if (got || !wait) return;
    u64 deadline = 0;
    {
        std::unique_lock lk(lib.m);
        if (!lib.initialized) return;
        const u64 now = now_us();
        deadline = wait < kForever ? now + wait : 0;
        const u64 due = next_deadline(lib);
        if (due && (!deadline || due < deadline)) deadline = std::max(due, now);
        sockets = normal_sockets(lib);
        if (sockets.empty()) {
            wait_change(lib, t, lk, deadline ? deadline : now + kIoTick);
            sockets = normal_sockets(lib);
        }
    }
    if (!sockets.empty()) {
        u8* base = t.kernel.base();
        const u32 fds = buf + kRxSize + 32, n = std::min<u32>(static_cast<u32>(sockets.size()), kPollSlots);
        for (u32 i = 0; i < n; i++) {
            ps_write32(base, fds + 8 * i, static_cast<u32>(sockets[i]));
            ps_write16(base, fds + 8 * i + 4, kPollin);
            ps_write16(base, fds + 8 * i + 6, 0);
        }
        const u64 now = now_us();
        const u64 left = deadline > now ? deadline - now : 0;
        const s32 ms = deadline ? static_cast<s32>(std::clamp<u64>((left + 999) / 1000, 1, 1'000'000)) : -1;
        lv2_syscall(ctx, kSysPoll, {fds, n, sext(ms)});
        {
            // A socket the game closed answers POLLNVAL at once: it is not read again (else this loop would spin).
            std::lock_guard lock(lib.m);
            for (u32 i = 0; i < n; i++) {
                if (!(ps_read16(base, fds + 8 * i + 6) & kPollnval)) continue;
                const auto it = lib.sockets.find(sockets[i]);
                if (it == lib.sockets.end() || it->second.closed) continue;
                it->second.closed = true;
                PS_LOG_WARN(kChan, "socket {} was closed while contexts use it: no longer read", sockets[i]);
            }
        }
        drain(ctx, lib, buf, sockets);
    }
    timers(ctx, lib);
}

// The internal I/O thread (cellRudpEnableInternalIOThread): the library's I/O until cellRudpEnd. Its argument is its
// guest memory block.
void _pspocket_rudp_io_thread(PPUContext& ctx, u64 arg)
{
    Lib& lib = lib_of();
    const u32 block = static_cast<u32>(arg);
    PS_LOG_INFO(kChan, "the internal I/O thread runs");
    while (!lib.io_stop.load(std::memory_order_acquire)) pump(ctx, lib, block, kIoTick);
    const Function* exit = registry().find("sysPrxForUser", recomp::nid_of("sys_ppu_thread_exit"));
    if (!exit) PS_FATAL(kChan, "sysPrxForUser::sys_ppu_thread_exit, which the I/O thread ends with, is not pspocket's");
    ctx.gpr[3] = 0;
    exit->fn(ctx, guest_base(ctx));
}

// ------------------------------------------------------------------------------------------------- contexts

std::shared_ptr<Context> find(Lib& lib, s32 id)
{
    const auto it = lib.contexts.find(id);
    return it == lib.contexts.end() ? nullptr : it->second;
}

s32 new_context_id(Lib& lib)
{
    for (;;) {
        const s32 id = lib.next_context;
        lib.next_context = lib.next_context == 0x7FFFFFFF ? 1 : lib.next_context + 1;
        if (!lib.contexts.contains(id)) return id;
    }
}

// The largest message a datagram context takes: its MAX_PAYLOAD, within a segment.
u32 max_message(const Lib& lib, const Context& c)
{
    const u32 seg = c.payload ? c.payload : lib.mss - kHeaderSize;
    return std::min(c.opt.max_payload, seg);
}

u16 ready_events(const Lib& lib, const Context& c)
{
    u16 r = 0;
    bool acceptable = false;
    for (const auto& k : c.pending) acceptable = acceptable || k->connected();
    if (!c.rcvq.empty() || c.state == kStateCloseWait || c.state == kStateClosed || acceptable) r |= kPollRead;
    const usize unit = c.p.stream ? 1 : max_message(lib, c);
    if ((c.state == kStateEstablished && c.queued() + unit <= c.opt.sndbuf) || c.state == kStateCloseWait ||
        c.state == kStateClosed)
        r |= kPollWrite;
    if (c.connected() && !c.queued()) r |= kPollFlush;
    if (c.state == kStateClosed && c.error) r |= kPollError;
    return r;
}

// The answer of a call that needs a connection, for a context that has none (0 when it has one, or is making one).
s32 not_connected(const Context& c)
{
    switch (c.state) {
    case kStateIdle: return error(CELL_RUDP_ERROR_NOT_BOUND);
    case kStateCloseWait: return error(CELL_RUDP_ERROR_END_OF_DATA);
    case kStateClosed: return c.error ? c.error : error(CELL_RUDP_ERROR_END_OF_DATA);
    default: return 0;
    }
}

// ------------------------------------------------------------------------------------------------- the API: life

// int cellRudpInit(CellRudpAllocator *allocator): NULL for the library's own (D-607).
s32 cellRudpInit(PPUContext& ctx, u32 allocator)
{
    Lib& lib = lib_of();
    std::lock_guard lock(lib.m);
    if (lib.initialized) return error(CELL_RUDP_ERROR_ALREADY_INITIALIZED);
    if (allocator && !guest_readable(thread_of(ctx), allocator, 8)) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    lib.initialized = true;
    lib.app_malloc = allocator ? ps_read32(guest_base(ctx), allocator) : 0;
    lib.app_free = allocator ? ps_read32(guest_base(ctx), allocator + 4) : 0;
    lib.handler = lib.handler_arg = 0;
    lib.mss = kDefaultMss;
    lib.stats = {};
    lib.records = 0;
    lib.io = false;
    PS_LOG_INFO(kChan, "cellRudpInit: {}", allocator ? "the game's allocator" : "the library's own allocator");
    return lv2::CELL_OK;
}

// int cellRudpEnd(void): every connection reset (its peer told), the I/O thread stopped, the contexts, polls and
// records gone, the allocator and handler forgotten. Threads waiting in the library return.
s32 cellRudpEnd(PPUContext& ctx)
{
    Lib& lib = lib_of();
    Work w;
    bool io = false;
    u32 io_thread = 0, io_block = 0;
    {
        std::lock_guard lock(lib.m);
        if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
        w = work_of(lib);
        const u64 now = now_us();
        each_connection(lib, [&](Context& c) {
            if (c.has_peer && (c.connected() || c.connecting()))
                send_control(lib, c, Type::Rst, now, w, rudp::kRstReset);
            if (c.record) w.records.push_back({c.record, c.record_free, c.record_own});
        });
        lib.contexts.clear();
        lib.lingering.clear();
        lib.sockets.clear();
        lib.polls.clear();
        lib.initialized = false;
        lib.app_malloc = lib.app_free = 0;
        lib.handler = lib.handler_arg = 0;
        io = lib.io;
        io_thread = lib.io_thread;
        io_block = lib.io_block;
        lib.io = false;
        lib.io_thread = lib.io_block = 0;
        lib.io_stop.store(true, std::memory_order_release);
        notify(lib);
    }
    // A handler the I/O called may end the library: the I/O thread then ends by itself, and the buffers it or
    // ProcessEvents still holds stay where they are.
    const bool from_io = io && lv2::sys_ppu_thread_get_id(thread_of(ctx)) == io_thread;
    if (io && !from_io) {
        StackVar status(ctx, guest_base(ctx), 16);
        lv2::sys_ppu_thread_join(thread_of(ctx), io_thread, status.addr());
        HleProcess::current()->main_memory().free(io_block);
    }
    finish(ctx, lib, w); // the resets (a RAW socket's through the handler as it was), the records; no other event
    if (!t_pumping) {
        std::lock_guard rx(lib.rx_m);
        if (lib.rx_block) HleProcess::current()->main_memory().free(lib.rx_block);
        lib.rx_block = 0;
    }
    PS_LOG_INFO(kChan, "cellRudpEnd");
    return lv2::CELL_OK;
}

// int cellRudpEnableInternalIOThread(uint32_t stack_size, uint32_t priority): the library's I/O on a PPU thread of its
// own (D-632). A second one THREAD_IN_USE; one that lv2 does not make THREAD.
s32 cellRudpEnableInternalIOThread(PPUContext& ctx, u32 stack_size, u32 priority)
{
    Lib& lib = lib_of();
    {
        std::lock_guard lock(lib.m);
        if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
        if (lib.io) return error(CELL_RUDP_ERROR_THREAD_IN_USE);
        lib.io = true;
    }
    const u32 stack = std::clamp<u32>(stack_size, 0x4000, 0x100000);
    const u32 prio = std::min<u32>(priority, 3071);
    if (stack != stack_size || prio != priority)
        PS_LOG_WARN(kChan, "cellRudpEnableInternalIOThread(stack {:#x}, priority {}): stack {:#x}, priority {}",
            stack_size, priority, stack, prio);
    const auto undo = [&](u32 code) {
        std::lock_guard lock(lib.m);
        lib.io = false;
        return error(code);
    };
    const u32 block = HleProcess::current()->main_memory().alloc(kIoBlockSize, 16);
    if (!block) return undo(CELL_RUDP_ERROR_MEMORY);
    const u32 entry = HleProcess::current()->link_function(kModule, recomp::nid_of("_pspocket_rudp_io_thread"),
        "_pspocket_rudp_io_thread");
    const Function* create = registry().find("sysPrxForUser", recomp::nid_of("sys_ppu_thread_create"));
    if (!entry || !create) {
        HleProcess::current()->main_memory().free(block);
        return undo(CELL_RUDP_ERROR_THREAD);
    }
    u8* base = guest_base(ctx);
    StackVar v(ctx, base, 0x20); // the thread ID, then its name
    std::memcpy(base + v.addr() + 8, "_cellRudpIOThread", 18);
    lib.io_stop.store(false, std::memory_order_release);
    u64 saved[7];
    for (u32 k = 0; k < 7; k++) saved[k] = ctx.gpr[3 + k];
    const u64 args[7] = {v.addr(), entry, block, prio, stack, 1, v.addr() + 8u};
    for (u32 k = 0; k < 7; k++) ctx.gpr[3 + k] = args[k];
    create->fn(ctx, base);
    const s32 r = static_cast<s32>(ctx.gpr[3]);
    for (u32 k = 0; k < 7; k++) ctx.gpr[3 + k] = saved[k];
    if (r != lv2::CELL_OK) {
        PS_LOG_WARN(kChan, "cellRudpEnableInternalIOThread: sys_ppu_thread_create gives {:#x}", static_cast<u32>(r));
        HleProcess::current()->main_memory().free(block);
        return undo(CELL_RUDP_ERROR_THREAD);
    }
    std::lock_guard lock(lib.m);
    lib.io_thread = static_cast<u32>(ps_read64(base, v.addr()));
    lib.io_block = block;
    return lv2::CELL_OK;
}

// int cellRudpSetEventHandler(CellRudpEventHandler handler, void *arg): NULL takes the handler away.
s32 cellRudpSetEventHandler(PPUContext&, u32 handler, u32 arg)
{
    Lib& lib = lib_of();
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    lib.handler = handler;
    lib.handler_arg = arg;
    return lv2::CELL_OK;
}

// int cellRudpSetMaxSegmentSize(uint16_t mss): the segment (header and payload in one datagram) of the connections
// started from now on.
s32 cellRudpSetMaxSegmentSize(PPUContext&, u16 mss)
{
    Lib& lib = lib_of();
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (mss < kMinMss || mss > kMaxDatagram) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    lib.mss = mss;
    return lv2::CELL_OK;
}

// int cellRudpGetMaxSegmentSize(uint16_t *mss)
s32 cellRudpGetMaxSegmentSize(PPUContext& ctx, u32 mss)
{
    Lib& lib = lib_of();
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (!guest_range(thread_of(ctx), mss, 2)) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    ps_write16(guest_base(ctx), mss, lib.mss);
    return lv2::CELL_OK;
}

// int cellRudpGetStatus(CellRudpStatus *status, size_t size): the library's counters (D-630's layout), as many bytes
// as `size` asks for, zeros past them.
s32 cellRudpGetStatus(PPUContext& ctx, u32 status, u32 size)
{
    Lib& lib = lib_of();
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    const u32 n = std::min(size, kStatusSize), all = std::min(size, 1024u);
    if (!size || !guest_range(thread_of(ctx), status, all)) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    note_memory(lib);
    lib.stats[kCurrentContexts] = total_contexts(lib);
    std::array<u8, kStatusSize> out{};
    for (u32 i = 0; i < kStatCount; i++) ps_st64(out.data() + 8 * i, lib.stats[i]);
    u8* base = guest_base(ctx);
    std::memcpy(base + status, out.data(), n);
    if (all > n) std::memset(base + status + n, 0, all - n);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- the API: contexts

// int cellRudpCreateContext(CellRudpContextEventHandler handler, void *arg, int *ctx_id): a context and its record
// from the allocator (MEMORY when it gives none). A NULL handler: no events.
s32 cellRudpCreateContext(PPUContext& ctx, u32 handler, u32 arg, u32 ctx_id)
{
    Lib& lib = lib_of();
    u32 app_malloc = 0, app_free = 0;
    {
        std::lock_guard lock(lib.m);
        if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
        if (!guest_range(thread_of(ctx), ctx_id, 4)) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        if (total_contexts(lib) >= kMaxContexts) return error(CELL_RUDP_ERROR_TOO_MANY_CONTEXTS);
        app_malloc = lib.app_malloc;
        app_free = lib.app_free;
    }
    const Record r = alloc_record(ctx, app_malloc, app_free);
    if (!r.addr) return error(CELL_RUDP_ERROR_MEMORY);
    std::unique_lock lock(lib.m);
    if (!lib.initialized || total_contexts(lib) >= kMaxContexts) {
        const s32 e = error(lib.initialized ? CELL_RUDP_ERROR_TOO_MANY_CONTEXTS : CELL_RUDP_ERROR_NOT_INITIALIZED);
        lock.unlock();
        free_record(ctx, r);
        return e;
    }
    auto c = std::make_shared<Context>();
    c->id = new_context_id(lib);
    c->handler = handler;
    c->arg = arg;
    c->record = r.addr;
    c->record_free = r.app_free;
    c->record_own = r.own;
    c->opt.max_payload = lib.mss - kHeaderSize;
    lib.contexts.emplace(c->id, c);
    lib.stats[kAllocs]++;
    lib.records += kRecordSize;
    note_memory(lib);
    ps_write32(guest_base(ctx), ctx_id, static_cast<u32>(c->id));
    PS_LOG_INFO(kChan, "cellRudpCreateContext: context {}", c->id);
    return lv2::CELL_OK;
}

// An option's value as a game gives it: 1, 2, 4 or 8 big-endian bytes.
std::optional<u64> option_value(lv2::PpuThread& t, u32 optval, u32 optlen)
{
    if ((optlen != 1 && optlen != 2 && optlen != 4 && optlen != 8) || !guest_range(t, optval, optlen))
        return std::nullopt;
    u8* base = t.kernel.base();
    switch (optlen) {
    case 1: return ps_read8(base, optval);
    case 2: return ps_read16(base, optval);
    case 4: return ps_read32(base, optval);
    default: return ps_read64(base, optval);
    }
}

// int cellRudpSetOption(int ctx_id, int option, const void *optval, size_t optlen): the value in 1, 2, 4 or 8 bytes
// (D-630). What a connection agrees on (STREAM, DELIVERY_CRITICAL, ORDER_CRITICAL) only before it starts.
s32 cellRudpSetOption(PPUContext& ctx, s32 ctx_id, s32 option, u32 optval, u32 optlen)
{
    Lib& lib = lib_of();
    lv2::PpuThread& t = thread_of(ctx);
    Work w;
    {
        std::lock_guard lock(lib.m);
        if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
        const auto c = find(lib, ctx_id);
        if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
        const std::optional<u64> v = option_value(t, optval, optlen);
        if (!v) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        Options& o = c->opt;
        const bool on = *v != 0;
        const auto fixed = [&]() -> s32 {
            if (c->state == kStateIdle) return 0;
            return error(c->connecting() ? CELL_RUDP_ERROR_IN_PROGRESS : CELL_RUDP_ERROR_ALREADY_ESTABLISHED);
        };
        switch (option) {
        case kOptMaxPayload:
            if (!*v) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
            if (*v > static_cast<u64>(lib.mss - kHeaderSize)) return error(CELL_RUDP_ERROR_PAYLOAD_TOO_LARGE);
            o.max_payload = static_cast<u32>(*v);
            break;
        case kOptSndbuf:
        case kOptRcvbuf:
            if (*v < kMinBuffer || *v > kMaxBuffer) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
            (option == kOptSndbuf ? o.sndbuf : o.rcvbuf) = static_cast<u32>(*v);
            break;
        case kOptNodelay: o.nodelay = on; break;
        case kOptNonblock: o.nonblock = on; break;
        case kOptDeliveryCritical:
        case kOptOrderCritical:
        case kOptStream:
            if (const s32 e = fixed()) return e;
            (option == kOptStream ? o.stream : option == kOptDeliveryCritical ? o.delivery : o.order) = on;
            break;
        case kOptConnectionTimeout: o.connection_timeout = *v; break;
        case kOptCloseWaitTimeout: o.close_wait_timeout = *v; break;
        case kOptAggregationTimeout: o.aggregation_timeout = *v; break;
        case kOptReadTimeout: o.read_timeout = *v; break;
        case kOptWriteTimeout: o.write_timeout = *v; break;
        case kOptFlushTimeout: o.flush_timeout = *v; break;
        case kOptKeepAliveInterval: o.keep_alive_interval = *v; break;
        case kOptKeepAliveTimeout: o.keep_alive_timeout = *v; break;
        default: return error(CELL_RUDP_ERROR_INVALID_OPTION); // LAST_ERROR is read-only
        }
        if (option == kOptNodelay || option == kOptRcvbuf || option == kOptAggregationTimeout) {
            w = work_of(lib);
            if (option == kOptRcvbuf && c->connected()) c->ack_now = true; // the window it has now
            push(lib, *c, now_us(), w);
            if (c->ack_now && c->connected()) send_control(lib, *c, Type::Ack, now_us(), w);
        }
    }
    finish(ctx, lib, w);
    return lv2::CELL_OK;
}

// int cellRudpGetOption(int ctx_id, int option, void *optval, size_t optlen): written in the width asked for (1, 2, 4
// or 8 bytes); BUFFER_TOO_SMALL when the value does not fit it.
s32 cellRudpGetOption(PPUContext& ctx, s32 ctx_id, s32 option, u32 optval, u32 optlen)
{
    Lib& lib = lib_of();
    lv2::PpuThread& t = thread_of(ctx);
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    const auto c = find(lib, ctx_id);
    if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    if ((optlen != 1 && optlen != 2 && optlen != 4 && optlen != 8) || !guest_range(t, optval, optlen))
        return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    const Options& o = c->opt;
    s64 v = 0;
    switch (option) {
    case kOptMaxPayload: v = o.max_payload; break;
    case kOptSndbuf: v = o.sndbuf; break;
    case kOptRcvbuf: v = o.rcvbuf; break;
    case kOptNodelay: v = o.nodelay; break;
    case kOptDeliveryCritical: v = o.delivery; break;
    case kOptOrderCritical: v = o.order; break;
    case kOptNonblock: v = o.nonblock; break;
    case kOptStream: v = o.stream; break;
    case kOptConnectionTimeout: v = static_cast<s64>(o.connection_timeout); break;
    case kOptCloseWaitTimeout: v = static_cast<s64>(o.close_wait_timeout); break;
    case kOptAggregationTimeout: v = static_cast<s64>(o.aggregation_timeout); break;
    case kOptLastError: v = c->error; break;
    case kOptReadTimeout: v = static_cast<s64>(o.read_timeout); break;
    case kOptWriteTimeout: v = static_cast<s64>(o.write_timeout); break;
    case kOptFlushTimeout: v = static_cast<s64>(o.flush_timeout); break;
    case kOptKeepAliveInterval: v = static_cast<s64>(o.keep_alive_interval); break;
    case kOptKeepAliveTimeout: v = static_cast<s64>(o.keep_alive_timeout); break;
    default: return error(CELL_RUDP_ERROR_INVALID_OPTION);
    }
    u8* base = t.kernel.base();
    const bool fits = optlen == 8 || (v >= 0 ? static_cast<u64>(v) >> (8 * optlen) == 0
                                             : optlen == 4 && v >= -0x80000000LL);
    if (!fits) return error(CELL_RUDP_ERROR_BUFFER_TOO_SMALL);
    switch (optlen) {
    case 1: ps_write8(base, optval, static_cast<u8>(v)); break;
    case 2: ps_write16(base, optval, static_cast<u16>(v)); break;
    case 4: ps_write32(base, optval, static_cast<u32>(v)); break;
    default: ps_write64(base, optval, static_cast<u64>(v)); break;
    }
    return lv2::CELL_OK;
}

// int cellRudpGetContextStatus(int ctx_id, CellRudpContextStatus *status, size_t size): D-630's 48 bytes (state,
// parentId, children, lostPackets, sentPackets, rcvdPackets, then u64 sentBytes and rcvdBytes, retransmissions, rtt in
// microseconds), as many as `size` asks for, zeros past them.
s32 cellRudpGetContextStatus(PPUContext& ctx, s32 ctx_id, u32 status, u32 size)
{
    Lib& lib = lib_of();
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    const auto c = find(lib, ctx_id);
    if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    const u32 n = std::min(size, kContextStatusSize), all = std::min(size, 256u);
    if (!size || !guest_range(thread_of(ctx), status, all)) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    std::array<u8, kContextStatusSize> out{};
    ps_st32(out.data(), c->state);
    ps_st32(out.data() + 4, static_cast<u32>(c->parent_id));
    ps_st32(out.data() + 8, c->listening ? static_cast<u32>(c->pending.size()) + c->children : 0);
    ps_st32(out.data() + 12, c->lost);
    ps_st32(out.data() + 16, c->sent_packets);
    ps_st32(out.data() + 20, c->rcvd_packets);
    ps_st64(out.data() + 24, c->sent_bytes);
    ps_st64(out.data() + 32, c->rcvd_bytes);
    ps_st32(out.data() + 40, c->retransmissions);
    ps_st32(out.data() + 44, static_cast<u32>(std::min<u64>(c->srtt, 0xFFFFFFFF)));
    u8* base = guest_base(ctx);
    std::memcpy(base + status, out.data(), n);
    if (all > n) std::memset(base + status + n, 0, all - n);
    return lv2::CELL_OK;
}

// int cellRudpGetLocalInfo(int ctx_id, int *soc, sys_net_sockaddr *addr, socklen_t *addrlen, uint16_t *vport,
// uint8_t *muxmode): each output that is given; the address is the socket's (getsockname), *addrlen its length in
// (the room) and out (16). NOT_BOUND before cellRudpBind.
s32 cellRudpGetLocalInfo(PPUContext& ctx, s32 ctx_id, u32 soc, u32 addr, u32 addrlen, u32 vport, u32 muxmode)
{
    Lib& lib = lib_of();
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    const auto c = find(lib, ctx_id);
    if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    if ((soc && !guest_range(t, soc, 4)) || (addr && (!addrlen || !guest_range(t, addrlen, 4))) ||
        (addrlen && !guest_range(t, addrlen, 4)) || (vport && !guest_range(t, vport, 2)) ||
        (muxmode && !guest_range(t, muxmode, 1)))
        return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    if (!c->bound) return error(CELL_RUDP_ERROR_NOT_BOUND);
    if (addr) {
        const u32 room = std::min(ps_read32(base, addrlen), kSockaddrSize);
        if (!guest_range(t, addr, room)) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        StackVar name(ctx, base, 32);
        ps_write32(base, name.addr() + 16, kSockaddrSize);
        lv2_syscall(ctx, kSysGetsockname, {sext(c->soc), name.addr(), name.addr() + 16});
        std::memcpy(base + addr, base + name.addr(), room);
    }
    if (addrlen) ps_write32(base, addrlen, kSockaddrSize);
    if (soc) ps_write32(base, soc, static_cast<u32>(c->soc));
    if (vport) ps_write16(base, vport, c->vport);
    if (muxmode) ps_write8(base, muxmode, c->mux);
    return lv2::CELL_OK;
}

// int cellRudpGetRemoteInfo(int ctx_id, sys_net_sockaddr *addr, socklen_t *addrlen, uint16_t *vport): the peer's
// address as the game gave it (or as its packets came) and vport; zeros before there is one.
s32 cellRudpGetRemoteInfo(PPUContext& ctx, s32 ctx_id, u32 addr, u32 addrlen, u32 vport)
{
    Lib& lib = lib_of();
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    const auto c = find(lib, ctx_id);
    if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    if ((addr && (!addrlen || !guest_range(t, addrlen, 4))) || (addrlen && !guest_range(t, addrlen, 4)) ||
        (vport && !guest_range(t, vport, 2)))
        return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    if (addr) {
        const u32 room = std::min(ps_read32(base, addrlen), kSockaddrSize);
        if (!guest_range(t, addr, room)) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        const std::array<u8, 16> zero{};
        std::memcpy(base + addr, c->has_peer ? c->peer.raw.data() : zero.data(), room);
    }
    if (addrlen) ps_write32(base, addrlen, kSockaddrSize);
    if (vport) ps_write16(base, vport, c->peer_vport);
    return lv2::CELL_OK;
}

// int cellRudpBind(int ctx_id, int soc, uint16_t vport, uint8_t muxmode): the context on the game's UDP or UDP P2P
// socket at a virtual port (0: the lowest free one). A socket's contexts share its mux mode; RAW needs the event
// handler.
s32 cellRudpBind(PPUContext& ctx, s32 ctx_id, s32 soc, u16 vport, u8 muxmode)
{
    Lib& lib = lib_of();
    u8* base = guest_base(ctx);
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    const auto c = find(lib, ctx_id);
    if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    if (muxmode > kMuxRaw) return error(CELL_RUDP_ERROR_INVALID_MUXMODE);
    if (c->bound) return error(CELL_RUDP_ERROR_ALREADY_BOUND);
    // The socket's type, from lv2's socket information (sys_net_get_sockinfo_ex: lv2 keeps no SO_TYPE for a P2P
    // socket, as RPCS3's keeps none): the request block {s, 0, 0, the information, 1}, then the 80 bytes.
    StackVar info(ctx, base, kSockinfoBlock + kSockinfoExSize);
    ps_write32(base, info.addr(), static_cast<u32>(soc));
    ps_write32(base, info.addr() + 12, info.addr() + kSockinfoBlock);
    ps_write32(base, info.addr() + 16, 1);
    const s32 r = static_cast<s32>(lv2_syscall(ctx, kSysInfoctl, {kInfoctlSockinfoEx, info.addr()}));
    const s32 type = static_cast<s32>(ps_read32(base, info.addr() + kSockinfoBlock + 36));
    if (r < 0 || (type != kSockDgram && type != kSockDgramP2p)) return error(CELL_RUDP_ERROR_INVALID_SOCKET);
    if (muxmode == kMuxRaw && !lib.handler) return error(CELL_RUDP_ERROR_NO_EVENT_HANDLER);
    const auto existing = lib.sockets.find(soc);
    if (existing != lib.sockets.end() && existing->second.mux != muxmode) return error(CELL_RUDP_ERROR_INVALID_MUXMODE);
    Binding& b = lib.sockets[soc];
    b.mux = muxmode;
    b.p2p = type == kSockDgramP2p;
    b.closed = false; // open: lv2 may hand a closed socket's number out again
    if (!vport) {
        for (u32 v = 1; v <= 0xFFFF && !vport; v++)
            if (!b.ports.contains(static_cast<u16>(v))) vport = static_cast<u16>(v);
        if (!vport) {
            if (b.ports.empty() && b.conns.empty()) lib.sockets.erase(soc);
            return error(CELL_RUDP_ERROR_VPORT_EXHAUSTED);
        }
    } else if (b.ports.contains(vport)) {
        return error(CELL_RUDP_ERROR_VPORT_IN_USE);
    }
    c->bound = true;
    c->soc = soc;
    c->vport = vport;
    c->mux = muxmode;
    b.ports[vport] = c;
    PS_LOG_INFO(kChan, "cellRudpBind: context {} on socket {} vport {}{}", ctx_id, soc, vport,
        muxmode == kMuxRaw ? " (RAW)" : "");
    notify(lib); // the I/O thread reads a new socket
    return lv2::CELL_OK;
}

// int cellRudpListen(int ctx_id, int backlog): the bound context takes SYNs from any peer at its vport, each a
// connection of its own for cellRudpAccept, up to `backlog` waiting (8 for 0 or less, at most 64).
s32 cellRudpListen(PPUContext&, s32 ctx_id, s32 backlog)
{
    Lib& lib = lib_of();
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    const auto c = find(lib, ctx_id);
    if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    if (!c->bound) return error(CELL_RUDP_ERROR_NOT_BOUND);
    if (c->state != kStateIdle || c->has_peer)
        return error(c->connecting() ? CELL_RUDP_ERROR_IN_PROGRESS : CELL_RUDP_ERROR_ALREADY_ESTABLISHED);
    c->listening = true;
    c->backlog = backlog <= 0 ? kDefaultBacklog : std::min<u32>(static_cast<u32>(backlog), kMaxBacklog);
    return lv2::CELL_OK;
}

// int cellRudpAccept(int ctx_id): the listener's oldest connection that finished its handshake, as a context of its
// own (its ID the answer; the listener's handler, argument and options). Without one: WOULDBLOCK when non-blocking,
// else a wait (READ_TIMEOUT).
s32 cellRudpAccept(PPUContext& ctx, s32 ctx_id)
{
    Lib& lib = lib_of();
    lv2::PpuThread& t = thread_of(ctx);
    std::unique_lock lk(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    auto l = find(lib, ctx_id);
    if (!l) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    if (!l->listening) return error(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    const u64 deadline = deadline_after(l->opt.read_timeout);
    for (;;) {
        const auto it = std::find_if(l->pending.begin(), l->pending.end(),
            [](const std::shared_ptr<Context>& k) { return k->connected(); });
        if (it != l->pending.end()) {
            if (total_contexts(lib) > kMaxContexts) return error(CELL_RUDP_ERROR_TOO_MANY_CONTEXTS);
            std::shared_ptr<Context> k = *it;
            l->pending.erase(it);
            const u32 app_malloc = lib.app_malloc, app_free = lib.app_free;
            lk.unlock();
            const Record r = alloc_record(ctx, app_malloc, app_free);
            lk.lock();
            Work w = work_of(lib);
            if (!r.addr || !lib.initialized) {
                if (lib.initialized) {
                    send_control(lib, *k, Type::Rst, now_us(), w, rudp::kRstReset);
                    unlink(lib, *k);
                }
                lk.unlock();
                finish(ctx, lib, w);
                free_record(ctx, r);
                return error(r.addr ? CELL_RUDP_ERROR_NOT_INITIALIZED : CELL_RUDP_ERROR_MEMORY);
            }
            k->id = new_context_id(lib);
            k->record = r.addr;
            k->record_free = r.app_free;
            k->record_own = r.own;
            k->parent_id = l->id;
            k->parent.reset();
            l->children++;
            lib.contexts.emplace(k->id, k);
            lib.stats[kAllocs]++;
            lib.records += kRecordSize;
            PS_LOG_INFO(kChan, "cellRudpAccept: context {} from listener {}", k->id, ctx_id);
            return k->id;
        }
        if (l->opt.nonblock) return error(CELL_RUDP_ERROR_WOULDBLOCK);
        if (!wait_change(lib, t, lk, deadline)) return error(CELL_RUDP_ERROR_WOULDBLOCK);
        if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
        l = find(lib, ctx_id);
        if (!l || !l->listening) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    }
}

// The peer's address of Initiate and Activate: a sys_net_sockaddr of at least 16 bytes.
std::optional<Endpoint> peer_arg(lv2::PpuThread& t, u32 to, u32 tolen)
{
    if (!to || tolen < kSockaddrSize || !guest_range(t, to, kSockaddrSize)) return std::nullopt;
    return endpoint_of(t.kernel.base() + to);
}

// What stops a context from starting a connection (0: nothing).
s32 cannot_start(const Context& c)
{
    if (!c.bound) return error(CELL_RUDP_ERROR_NOT_BOUND);
    if (c.listening) return error(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    if (c.connecting()) return error(CELL_RUDP_ERROR_IN_PROGRESS);
    if (c.state != kStateIdle || c.has_peer) return error(CELL_RUDP_ERROR_ALREADY_ESTABLISHED);
    return 0;
}

// Initiate's and Activate's connection: its peer, the SYN.
s32 open(PPUContext& ctx, s32 ctx_id, u32 to, u32 tolen, u16 vport, bool active)
{
    Lib& lib = lib_of();
    lv2::PpuThread& t = thread_of(ctx);
    Work w;
    {
        std::lock_guard lock(lib.m);
        if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
        const auto c = find(lib, ctx_id);
        if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
        if (const s32 e = cannot_start(*c)) return e;
        const std::optional<Endpoint> peer = peer_arg(t, to, tolen);
        if (!peer) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        if (active && !vport) return error(CELL_RUDP_ERROR_INVALID_VPORT);
        w = work_of(lib);
        Binding& b = lib.sockets[c->soc];
        for (const auto& old : std::vector(lib.lingering)) // a connection this side let go to the same place
            if (old->soc == c->soc && old->vport == c->vport && same_peer(old->peer, *peer, b.p2p) &&
                (!active || old->peer_vport == vport))
                drop_lingering(lib, *old, w);
        const u64 now = now_us();
        c->peer = *peer;
        c->peer_vport = active ? vport : 0;
        c->activated = !active;
        start(lib, *c, now);
        c->state = kStateSynSent;
        link(lib, c);
        send_syn(lib, *c, now, w);
        PS_LOG_INFO(kChan, "{}: context {} vport {} to {}.{}.{}.{}:{} vport {}",
            active ? "cellRudpInitiate" : "cellRudpActivate", ctx_id, c->vport, peer->addr >> 24,
            (peer->addr >> 16) & 0xFF, (peer->addr >> 8) & 0xFF, peer->addr & 0xFF, peer->port,
            active ? vport : c->vport);
    }
    finish(ctx, lib, w);
    return lv2::CELL_OK;
}

// int cellRudpInitiate(int ctx_id, const sys_net_sockaddr *to, socklen_t tolen, uint16_t vport): a connection to the
// peer's vport (D-630), which it takes with Listen, Activate or an Initiate of its own (the two SYNs meet).
s32 cellRudpInitiate(PPUContext& ctx, s32 ctx_id, u32 to, u32 tolen, u16 vport)
{
    return open(ctx, ctx_id, to, tolen, vport, true);
}

// int cellRudpActivate(int ctx_id, const sys_net_sockaddr *to, socklen_t tolen): a connection with this peer only,
// from whichever vport its SYN comes; this side's SYNs go to its own vport and are dropped, not refused, where nothing
// there waits for them (two peers activating each other at the same vport meet) (D-630).
s32 cellRudpActivate(PPUContext& ctx, s32 ctx_id, u32 to, u32 tolen) { return open(ctx, ctx_id, to, tolen, 0, false); }

// int cellRudpTerminate(int ctx_id): the context goes. A connection sends what was written, then its end (FIN), and
// lingers until its FIN and the peer's are acknowledged, at most CLOSE_WAIT_TIMEOUT (then a RST); one still being made
// is reset, and so are a listener's connections not accepted.
s32 cellRudpTerminate(PPUContext& ctx, s32 ctx_id)
{
    Lib& lib = lib_of();
    Work w;
    {
        std::lock_guard lock(lib.m);
        if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
        const auto c = find(lib, ctx_id);
        if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
        w = work_of(lib);
        const u64 now = now_us();
        lib.contexts.erase(ctx_id);
        for (auto& [id, poll] : lib.polls)
            std::erase_if(poll.entries, [&](const std::pair<s32, u16>& e) { return e.first == ctx_id; });
        for (const auto& k : std::vector(c->pending)) {
            send_control(lib, *k, Type::Rst, now, w, rudp::kRstReset);
            unlink(lib, *k);
        }
        c->pending.clear();
        if (c->record) {
            w.records.push_back({c->record, c->record_free, c->record_own});
            lib.stats[kFrees]++;
            lib.records -= kRecordSize;
            c->record = 0;
        }
        if (c->bound) {
            Binding& b = lib.sockets[c->soc];
            const auto port = b.ports.find(c->vport);
            if (port != b.ports.end() && port->second == c) b.ports.erase(port);
        }
        if (c->parent_id)
            if (const auto l = find(lib, c->parent_id); l && l->children) l->children--;
        if (c->connected()) { // its end after what it wrote; lingering until both ends' FINs are acknowledged
            c->id = 0;
            c->lingering = true;
            c->linger_deadline = now + c->opt.close_wait_timeout;
            c->fin_wanted = true;
            if (!c->p.stream) {
                Segment s;
                s.type = Type::Fin;
                s.seq = c->snd_nxt++;
                s.offset = c->snd_offset;
                c->sndq.push_back(std::move(s));
                c->fin_queued = true;
            }
            lib.lingering.push_back(c);
            push(lib, *c, now, w);
        } else {
            if (c->connecting()) send_control(lib, *c, Type::Rst, now, w, rudp::kRstReset);
            if (c->has_peer) unlink(lib, *c);
        }
        if (c->bound) release_if_unused(lib, c->soc, w);
        note_memory(lib);
        notify(lib);
        PS_LOG_INFO(kChan, "cellRudpTerminate: context {}", ctx_id);
    }
    finish(ctx, lib, w);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- the API: data

void write_read_info(u8* base, u32 info, const Chunk& c)
{
    // CellRudpReadInfo (D-630, 12 bytes): size, retransmissionCount, retransmissionDelay, retransmissionDelay2,
    // flags, sequenceNumber, timestamp.
    ps_write8(base, info, kReadInfoSize);
    ps_write8(base, info + 1, c.retries);
    ps_write16(base, info + 2, 0);
    ps_write8(base, info + 4, 0);
    ps_write8(base, info + 5, c.flags & rudp::kFlagLatency ? kMsgLatencyCritical : 0);
    ps_write16(base, info + 6, static_cast<u16>(c.seq));
    ps_write32(base, info + 8, c.stamp);
}

// int cellRudpRead(int ctx_id, void *data, size_t len, uint8_t flags, CellRudpReadInfo *info): a message (datagram
// contexts; BUFFER_TOO_SMALL, the message kept, when it does not fit) or the bytes there are, up to len (a stream).
// With nothing to read: WOULDBLOCK when non-blocking (NONBLOCK, CELL_RUDP_MSG_DONTWAIT), else a wait (READ_TIMEOUT);
// END_OF_DATA once the peer's end is read; the error that closed the connection.
s32 cellRudpRead(PPUContext& ctx, s32 ctx_id, u32 data, u32 len, u8 flags, u32 info)
{
    Lib& lib = lib_of();
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    std::unique_lock lk(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    auto c = find(lib, ctx_id);
    if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    if (!len || !guest_range(t, data, len) || (info && !guest_range(t, info, kReadInfoSize)))
        return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    if (c->listening) return error(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    const bool nonblock = c->opt.nonblock || (flags & kMsgDontwait);
    const u64 deadline = deadline_after(c->opt.read_timeout);
    for (;;) {
        if (!c->rcvq.empty()) {
            u32 n = 0;
            if (!c->p.stream) {
                const Chunk& m = c->rcvq.front();
                if (m.data.size() > len) return error(CELL_RUDP_ERROR_BUFFER_TOO_SMALL);
                n = static_cast<u32>(m.data.size());
                std::memcpy(base + data, m.data.data(), n);
                if (info) write_read_info(base, info, m);
                c->rcv_bytes -= n;
                c->rcvq.pop_front();
            } else {
                if (info) write_read_info(base, info, c->rcvq.front());
                while (n < len && !c->rcvq.empty()) {
                    Chunk& m = c->rcvq.front();
                    const u32 k = static_cast<u32>(std::min<usize>(len - n, m.data.size() - m.read));
                    std::memcpy(base + data + n, m.data.data() + m.read, k);
                    m.read += k;
                    n += k;
                    c->rcv_bytes -= k;
                    if (m.read == m.data.size()) c->rcvq.pop_front();
                }
            }
            // A window that opened by a segment (or half the buffer, when that is less) is told at once (RFC 1122's
            // rule against silly windows).
            Work w = work_of(lib);
            const u32 step = std::min(std::max(c->payload, 1u), std::max(c->opt.rcvbuf / 2, 1u));
            if (c->connected() && window_of(*c) >= c->last_window + step) send_control(lib, *c, Type::Ack, now_us(), w);
            note_memory(lib);
            lk.unlock();
            finish(ctx, lib, w);
            return static_cast<s32>(n);
        }
        if (const s32 e = not_connected(*c)) return e;
        if (nonblock) return error(CELL_RUDP_ERROR_WOULDBLOCK);
        if (!wait_change(lib, t, lk, deadline)) return error(CELL_RUDP_ERROR_WOULDBLOCK);
        if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
        c = find(lib, ctx_id);
        if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    }
}

// int cellRudpWrite(int ctx_id, const void *data, size_t len, uint8_t flags): a message (datagram contexts: at most
// MAX_PAYLOAD, else MSG_TOO_LARGE; taken whole) or bytes (a stream: as many as the send buffer takes, all of them when
// blocking). Without room: WOULDBLOCK when non-blocking, else a wait (WRITE_TIMEOUT). Written before the handshake
// ends, it goes out once it does. LATENCY_CRITICAL sends at once.
s32 cellRudpWrite(PPUContext& ctx, s32 ctx_id, u32 data, u32 len, u8 flags)
{
    Lib& lib = lib_of();
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    std::unique_lock lk(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    auto c = find(lib, ctx_id);
    if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    if (!len || !guest_range(t, data, len)) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    if (c->listening) return error(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    if (const s32 e = not_connected(*c)) return e;
    if (!c->p.stream && (len > max_message(lib, *c) || len > c->opt.sndbuf))
        return error(CELL_RUDP_ERROR_MSG_TOO_LARGE);
    const bool nonblock = c->opt.nonblock || (flags & kMsgDontwait);
    const bool latency = flags & kMsgLatencyCritical;
    const u64 deadline = deadline_after(c->opt.write_timeout);
    u32 written = 0;
    for (;;) {
        const usize queued = c->queued();
        const usize room = c->opt.sndbuf > queued ? c->opt.sndbuf - queued : 0;
        u32 n = 0;
        if (!c->p.stream) {
            if (len <= room) {
                Segment s;
                s.seq = c->snd_nxt++;
                s.flags = latency ? rudp::kFlagLatency : 0;
                s.data.assign(base + data, base + data + len);
                s.offset = c->snd_offset;
                c->snd_offset += len;
                c->sndq.push_back(std::move(s));
                n = len;
            }
        } else {
            n = static_cast<u32>(std::min<usize>(len - written, room));
            c->stream_out.insert(c->stream_out.end(), base + data + written, base + data + written + n);
        }
        if (n) {
            written += n;
            c->queued_bytes += n;
            const u32 level = quality_of(c->p);
            lib.stats[kSentUserBytes] += n;
            lib.stats[kQualityLevels + 4 * level] += n;
            if (!c->p.stream || written == len) {
                lib.stats[kSentUserPackets]++;
                lib.stats[kQualityLevels + 4 * level + 2]++;
                if (latency) lib.stats[kSentLatencyCriticalPackets]++;
            }
            Work w = work_of(lib);
            c->flush_now = c->flush_now || latency;
            push(lib, *c, now_us(), w);
            note_memory(lib);
            lk.unlock();
            finish(ctx, lib, w);
            if (written == len) return static_cast<s32>(written);
            lk.lock(); // a stream's bytes that did not fit yet
            if (!lib.initialized) return static_cast<s32>(written);
            c = find(lib, ctx_id);
            if (!c || not_connected(*c)) return static_cast<s32>(written);
            continue;
        }
        c->writable_wanted = true;
        if (nonblock) return written ? static_cast<s32>(written) : error(CELL_RUDP_ERROR_WOULDBLOCK);
        if (!wait_change(lib, t, lk, deadline))
            return written ? static_cast<s32>(written) : error(CELL_RUDP_ERROR_WOULDBLOCK);
        if (!lib.initialized) return written ? static_cast<s32>(written) : error(CELL_RUDP_ERROR_NOT_INITIALIZED);
        c = find(lib, ctx_id);
        if (!c) return written ? static_cast<s32>(written) : error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
        if (const s32 e = not_connected(*c)) return written ? static_cast<s32>(written) : e;
    }
}

// int cellRudpGetSizeReadable(int ctx_id): the bytes waiting to be read (every message's).
s32 cellRudpGetSizeReadable(PPUContext&, s32 ctx_id)
{
    Lib& lib = lib_of();
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    const auto c = find(lib, ctx_id);
    if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    return static_cast<s32>(std::min<usize>(c->rcv_bytes, 0x7FFFFFFF));
}

// int cellRudpGetSizeWritable(int ctx_id): the room left in the send buffer.
s32 cellRudpGetSizeWritable(PPUContext&, s32 ctx_id)
{
    Lib& lib = lib_of();
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    const auto c = find(lib, ctx_id);
    if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    const usize q = c->queued();
    return static_cast<s32>(c->opt.sndbuf > q ? c->opt.sndbuf - q : 0);
}

// int cellRudpFlush(int ctx_id): what was written goes out now (no aggregation), and the call waits until the peer
// has it all (FLUSH_TIMEOUT); non-blocking, WOULDBLOCK until then, and a FLUSHED event when it has.
s32 cellRudpFlush(PPUContext& ctx, s32 ctx_id)
{
    Lib& lib = lib_of();
    lv2::PpuThread& t = thread_of(ctx);
    std::unique_lock lk(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    auto c = find(lib, ctx_id);
    if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    if (const s32 e = not_connected(*c); e && c->state != kStateCloseWait) return e;
    Work w = work_of(lib);
    c->flush_now = true;
    push(lib, *c, now_us(), w);
    lk.unlock();
    finish(ctx, lib, w);
    lk.lock();
    const u64 deadline = deadline_after(c->opt.flush_timeout);
    for (;;) {
        if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
        c = find(lib, ctx_id);
        if (!c) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
        if (!c->queued()) return lv2::CELL_OK;
        if (c->state == kStateClosed) return not_connected(*c);
        if (c->opt.nonblock) {
            c->flush_wanted = true;
            return error(CELL_RUDP_ERROR_WOULDBLOCK);
        }
        if (!wait_change(lib, t, lk, deadline)) return error(CELL_RUDP_ERROR_WOULDBLOCK);
    }
}

// ------------------------------------------------------------------------------------------------- the API: polling

// int cellRudpPollCreate(size_t size): a poll set for up to `size` contexts; its ID the answer.
s32 cellRudpPollCreate(PPUContext&, u32 size)
{
    Lib& lib = lib_of();
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (!size || size > kMaxPollSize) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    if (lib.polls.size() >= kMaxPolls) return error(CELL_RUDP_ERROR_MEMORY);
    s32 id = 0;
    while (!id || lib.polls.contains(id)) {
        id = lib.next_poll;
        lib.next_poll = lib.next_poll == 0x7FFFFFFF ? 1 : lib.next_poll + 1;
    }
    lib.polls[id].size = size;
    return id;
}

// int cellRudpPollDestroy(int poll_id): a wait on it ends with CANCELLED.
s32 cellRudpPollDestroy(PPUContext&, s32 poll_id)
{
    Lib& lib = lib_of();
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (!lib.polls.erase(poll_id)) return error(CELL_RUDP_ERROR_INVALID_POLL_ID);
    notify(lib);
    return lv2::CELL_OK;
}

// int cellRudpPollControl(int poll_id, int op, int ctx_id, uint16_t events): ADD (ALREADY_EXISTS twice; MEMORY past
// the set's size), MODIFY, REMOVE (INVALID_CONTEXT_ID for one not in the set).
s32 cellRudpPollControl(PPUContext&, s32 poll_id, s32 op, s32 ctx_id, u16 events)
{
    Lib& lib = lib_of();
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    const auto p = lib.polls.find(poll_id);
    if (p == lib.polls.end()) return error(CELL_RUDP_ERROR_INVALID_POLL_ID);
    if (!find(lib, ctx_id)) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    auto& entries = p->second.entries;
    const auto it = std::find_if(entries.begin(), entries.end(),
        [&](const std::pair<s32, u16>& e) { return e.first == ctx_id; });
    switch (op) {
    case kPollAdd:
        if (events & ~kPollEvents) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        if (it != entries.end()) return error(CELL_RUDP_ERROR_ALREADY_EXISTS);
        if (entries.size() >= p->second.size) return error(CELL_RUDP_ERROR_MEMORY);
        entries.emplace_back(ctx_id, events);
        break;
    case kPollModify:
        if (events & ~kPollEvents) return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        if (it == entries.end()) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
        it->second = events;
        break;
    case kPollRemove:
        if (it == entries.end()) return error(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
        entries.erase(it);
        break;
    default: return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    }
    notify(lib);
    return lv2::CELL_OK;
}

// int cellRudpPollWait(int poll_id, CellRudpPollEvent *events, size_t event_len, uint64_t timeout): the set's
// contexts with an event asked for (ERROR always), up to event_len of {ctx_id, reqEvents, rtnEvents} (8 bytes each);
// their number the answer. Timeout in microseconds: 0 answers at once, a wait that ends without one 0 (D-630).
// PollCancel or PollDestroy end a wait with CANCELLED.
s32 cellRudpPollWait(PPUContext& ctx, s32 poll_id, u32 events, u32 event_len, u64 timeout)
{
    Lib& lib = lib_of();
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    std::unique_lock lk(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    auto p = lib.polls.find(poll_id);
    if (p == lib.polls.end()) return error(CELL_RUDP_ERROR_INVALID_POLL_ID);
    if (!event_len || event_len > kMaxPollSize || !guest_range(t, events, u64{event_len} * kPollEventSize))
        return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    const u64 cancels = p->second.cancels;
    const u64 deadline = deadline_after(timeout);
    for (;;) {
        u32 n = 0;
        for (const auto& [id, want] : p->second.entries) {
            if (n == event_len) break;
            const auto c = find(lib, id);
            if (!c) continue;
            const u16 got = ready_events(lib, *c) & (want | kPollError);
            if (!got) continue;
            const u32 e = events + kPollEventSize * n++;
            ps_write32(base, e, static_cast<u32>(id));
            ps_write16(base, e + 4, want);
            ps_write16(base, e + 6, got);
        }
        if (n || !timeout) return static_cast<s32>(n);
        if (!wait_change(lib, t, lk, deadline)) return 0;
        if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
        p = lib.polls.find(poll_id);
        if (p == lib.polls.end() || p->second.cancels != cancels) return error(CELL_RUDP_ERROR_CANCELLED);
    }
}

// int cellRudpPollCancel(int poll_id): the waits on the set end with CANCELLED.
s32 cellRudpPollCancel(PPUContext&, s32 poll_id)
{
    Lib& lib = lib_of();
    std::lock_guard lock(lib.m);
    if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    const auto p = lib.polls.find(poll_id);
    if (p == lib.polls.end()) return error(CELL_RUDP_ERROR_INVALID_POLL_ID);
    p->second.cancels++;
    notify(lib);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- the API: I/O

// int cellRudpNetReceived(int soc, const uint8_t *data, size_t datalen, const sys_net_sockaddr *addr, socklen_t
// addrlen): a datagram the game received on a socket a context uses, from addr (D-632). MSG_MALFORMED for one that is
// not RUDP; INVALID_SOCKET for a socket no context uses.
s32 cellRudpNetReceived(PPUContext& ctx, s32 soc, u32 data, u32 datalen, u32 addr, u32 addrlen)
{
    Lib& lib = lib_of();
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    Work w;
    {
        std::lock_guard lock(lib.m);
        if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
        if (!datalen || datalen > 0xFFFF || !guest_range(t, data, datalen) || addrlen < kSockaddrSize ||
            !guest_range(t, addr, kSockaddrSize))
            return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        if (!lib.sockets.contains(soc)) return error(CELL_RUDP_ERROR_INVALID_SOCKET);
        w = work_of(lib);
        const std::vector<u8> bytes(base + data, base + data + datalen);
        if (!receive(lib, soc, endpoint_of(base + addr), bytes.data(), bytes.size(), now_us(), w))
            return error(CELL_RUDP_ERROR_MSG_MALFORMED);
    }
    finish(ctx, lib, w);
    return lv2::CELL_OK;
}

// int cellRudpProcessEvents(uint64_t timeout): the library's I/O on the calling thread (D-632): the NORMAL-mode
// sockets read, the timers due run, the handlers called; with a timeout (microseconds), up to that long for a datagram
// or a timer first when nothing was there.
s32 cellRudpProcessEvents(PPUContext& ctx, u64 timeout)
{
    Lib& lib = lib_of();
    {
        std::lock_guard lock(lib.m);
        if (!lib.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    }
    if (t_pumping) return error(CELL_RUDP_ERROR_THREAD_IN_USE); // from a handler the library's I/O called
    std::lock_guard rx(lib.rx_m);
    if (!lib.rx_block) lib.rx_block = HleProcess::current()->main_memory().alloc(kIoBlockSize, 16);
    if (!lib.rx_block) return error(CELL_RUDP_ERROR_MEMORY);
    pump(ctx, lib, lib.rx_block, timeout);
    return lv2::CELL_OK;
}

} // namespace

void rudp::set_send_filter(SendFilter filter)
{
    Lib& lib = lib_of();
    std::lock_guard lock(lib.filter_m);
    lib.filter = std::move(filter);
    lib.has_filter.store(static_cast<bool>(lib.filter), std::memory_order_release);
}

void register_cellRudp(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0x63F63545, cellRudpInit),
        PS_HLE_FUNC(0xB6BCB4A1, cellRudpEnd),
        PS_HLE_FUNC(0x6C0CFF03, cellRudpEnableInternalIOThread),
        PS_HLE_FUNC(0x7ED95E60, cellRudpSetEventHandler),
        PS_HLE_FUNC(0x54F81789, cellRudpSetMaxSegmentSize),
        PS_HLE_FUNC(0xFBF7E9E4, cellRudpGetMaxSegmentSize),
        PS_HLE_FUNC(0x7DADC739, cellRudpCreateContext),
        PS_HLE_FUNC(0x384BA777, cellRudpSetOption),
        PS_HLE_FUNC(0xFF9D259C, cellRudpGetOption),
        PS_HLE_FUNC(0x74BFAD12, cellRudpGetContextStatus),
        PS_HLE_FUNC(0xCD1A3F23, cellRudpGetStatus),
        PS_HLE_FUNC(0xD666931F, cellRudpGetLocalInfo),
        PS_HLE_FUNC(0x576831AE, cellRudpGetRemoteInfo),
        PS_HLE_FUNC(0x9AA6A321, cellRudpAccept),
        PS_HLE_FUNC(0xEE41E16A, cellRudpBind),
        PS_HLE_FUNC(0xB988502C, cellRudpListen),
        PS_HLE_FUNC(0xC407844F, cellRudpInitiate),
        PS_HLE_FUNC(0xC1AD7CED, cellRudpActivate),
        PS_HLE_FUNC(0x48D3EEAC, cellRudpTerminate),
        PS_HLE_FUNC(0x92E4D899, cellRudpRead),
        PS_HLE_FUNC(0x48C001B0, cellRudpWrite),
        PS_HLE_FUNC(0x2CDE989F, cellRudpGetSizeReadable),
        PS_HLE_FUNC(0xA86B28E3, cellRudpGetSizeWritable),
        PS_HLE_FUNC(0xA70737DA, cellRudpFlush),
        PS_HLE_FUNC(0x6BC587E9, cellRudpPollCreate),
        PS_HLE_FUNC(0x8AC398F1, cellRudpPollDestroy),
        PS_HLE_FUNC(0xA3DB855C, cellRudpPollControl),
        PS_HLE_FUNC(0xD8310700, cellRudpPollWait),
        PS_HLE_FUNC(0xDF5A2432, cellRudpPollCancel),
        PS_HLE_FUNC(0x6EE04954, cellRudpNetReceived),
        PS_HLE_FUNC(0xFADE48B2, cellRudpProcessEvents),
        PS_HLE_INTERNAL(0x03024A36, _pspocket_rudp_io_thread),
    });
    r.firmware_first(kModule);
}

} // namespace pspocket::hle
