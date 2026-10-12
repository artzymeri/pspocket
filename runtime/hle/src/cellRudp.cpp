// cellRudp: Sony's reliable UDP for games' peer-to-peer play (librudp), pspocket's own (D-607, D-630 to D-633), made
// to do what Sony's librudp does call for call (D-916): its answers and checks, its options, its structures, its
// events and when they come, its wire format (rudp.hpp; a peer may be a real PS3), its timers, retransmissions,
// congestion control and statistics. Read from Sony's module (firmware 4.92's librudp.sprx); nothing of it is copied
// here: the code below is pspocket's own, written from what the module was understood to do.
//
// How librudp is built, and so this library:
// - One lock (a recursive lightweight mutex) around everything: every call takes it, the internal I/O thread takes it
//   to process what it received, and the game's handlers run with it held (they may call the library again).
// - A scheduler: an event queue and timers. What a connection has to tell (established, readable, closed, a packet to
//   send...) is queued as an event and handled later, when cellRudpProcessEvents runs the scheduler or the internal
//   I/O thread does (about every 16.7 ms, and at once when it is woken). Each context has a timer (every 16 ms at
//   first, then every half round trip, 125 ms before the first sample) that runs its retransmissions, delayed
//   acknowledgements, window probes and keep-alive.
// - The socket I/O: without the internal I/O thread the library never touches a socket: what it sends goes to the
//   event handler (CELL_RUDP_EVENT_SEND), what the game receives it gives to cellRudpNetReceived. With the thread,
//   the library sends with sendto (the handler first hears event 100, and may refuse) and the thread reads the
//   sockets with socketselect and recvfrom (the handler first hears event 101). Those socket calls go through libnet
//   (sys_net's exports, librudp's imports), so they fail as Sony's do until the game has brought the network up.
//   Packets to one destination are aggregated into one datagram for up to the aggregation timeout.
// - Its clock is the wall clock (sys_time_get_current_time): the timers and the time stamps a write may carry.
// - Contexts: a connection each, over a socket bound in mux mode 1 (P2P: the peers share one vport) or 2 (each side
//   its own vport; listeners). A listener answers a SYN with a cookie (a RST carrying 32 bytes) unless option 13 (the
//   cookie's lifetime in seconds) is 0; a SYN echoing a valid cookie makes a child context that completes the
//   handshake and waits to be accepted.
//
// Reference for the names only: RPCS3 rpcs3/Emu/Cell/Modules/cellRudp.h (the error codes, the options, the poll
// flags, CellRudpAllocator, the event handler's arguments). Everything else is librudp's behaviour.

#include "rudp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
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
#include "pspocket/recomp/hash.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {

// ------------------------------------------------------------------------------------------------- the wire format

namespace rudp {

void encode(const Header& h, std::vector<u8>& out, u32 base_align)
{
    const usize start = out.size();
    const auto w8 = [&](u32 v) { out.push_back(static_cast<u8>(v)); };
    const auto w16 = [&](u32 v) {
        w8(v >> 8);
        w8(v);
    };
    const auto w32 = [&](u32 v) {
        w16(v >> 16);
        w16(v);
    };
    w8(static_cast<u32>(h.type << 6) | (h.flags & 0x3F));
    w8(h.window);
    w16(h.seq);
    if (h.flags & kFlagAck) {
        w16(h.ack);
        if (h.flags & kFlagDelay) w16(h.delay);
    }
    if (h.type == kTypeSyn) {
        w16((h.syn & kSynStream ? 0x800u : 0u) | (h.syn & kSynDelivery ? 0x200u : 0u) |
            (h.syn & kSynOrder ? 0x100u : 0u) | (h.level & 0xFu));
        w16(h.version);
        w16(h.lowest);
        w32(h.token);
        w16(h.mss);
    } else if (h.type == kTypeRst) {
        w16(h.code);
        w32(h.token);
    }
    std::optional<usize> last; // the last option's kind byte
    if (h.opts & kOptCookie) {
        last = out.size();
        w8(1);
        w8(h.cookie_len);
        out.insert(out.end(), h.cookie.begin(), h.cookie.begin() + std::min<u32>(h.cookie_len, 32));
    }
    if (h.type != kTypeSyn && (h.opts & kOptMss)) {
        last = out.size();
        w8(2);
        w8(2);
        w16(h.mss);
    }
    if (h.nsack) {
        last = out.size();
        w8(5);
        w8((h.nsack & 0x3F) << 2);
        for (u32 i = 0; i < h.nsack && i < 8; i++) {
            w16(h.sack[i][0]);
            w16(h.sack[i][1]);
        }
    }
    if (h.ext & kExtRetransmission) {
        last = out.size();
        w8(10);
        w8(4);
        w32(h.retransmission);
    }
    if (h.ext & kExtTimestamp) {
        last = out.size();
        w8(11);
        w8(4);
        w32(h.stamp);
    }
    if (h.align) {
        const u32 a = h.align, at = (base_align + static_cast<u32>(out.size())) & (a - 1);
        const u32 pad = (a - 1) & (a - at) & 0xFF;
        for (u32 k = 0; k < pad; k++) {
            last = out.size();
            w8(0);
        }
    }
    if (last) {
        out[start] |= kFlagOptions;
        out[*last] |= 0x80;
    }
}

std::optional<u32> decode(const u8* p, u32 n, Header& h)
{
    h = Header{};
    u32 at = 0;
    const auto need = [&](u32 k) { return n - at >= k; };
    const auto r8 = [&] { return p[at++]; };
    const auto r16 = [&] {
        const u16 v = static_cast<u16>(p[at] << 8 | p[at + 1]);
        at += 2;
        return v;
    };
    const auto r32 = [&] {
        const u32 v = static_cast<u32>(p[at]) << 24 | static_cast<u32>(p[at + 1]) << 16 |
                      static_cast<u32>(p[at + 2]) << 8 | p[at + 3];
        at += 4;
        return v;
    };
    if (!need(1)) return std::nullopt;
    const u8 b0 = r8();
    h.type = b0 >> 6;
    h.flags = b0 & 0x3F;
    if (!need(1)) return std::nullopt;
    h.window = r8();
    if (!need(2)) return std::nullopt;
    h.seq = r16();
    if (h.flags & kFlagAck) {
        if (!need(2)) return std::nullopt;
        h.ack = r16();
    }
    if (h.flags & kFlagDelay) {
        if (!(h.flags & kFlagAck) || !need(2)) return std::nullopt;
        h.delay = r16();
    }
    if (h.type == kTypeSyn) {
        if (!need(12)) return std::nullopt;
        const u16 params = r16();
        h.syn = static_cast<u8>((params & 0x800 ? kSynStream : 0) | (params & 0x200 ? kSynDelivery : 0) |
                                (params & 0x100 ? kSynOrder : 0));
        h.level = params & 0xF;
        h.version = r16();
        h.lowest = r16();
        h.token = r32();
        h.mss = r16();
        h.opts |= kOptMss;
    } else if (h.type == kTypeRst) {
        if (!need(6)) return std::nullopt;
        h.code = static_cast<u8>(r16());
        h.token = r32();
    }
    if (h.flags & kFlagOptions) {
        while (n != at) {
            u8 kind = r8();
            const bool final = kind & 0x80;
            kind &= 0x7F;
            if (kind) {
                if (!need(1)) return std::nullopt;
                const u8 len = r8();
                if (n - at < len) return std::nullopt;
                switch (kind) {
                case 1:
                    if (static_cast<u8>(len - 1) > 31) return std::nullopt;
                    h.cookie_len = len;
                    std::memcpy(h.cookie.data(), p + at, len);
                    at += len;
                    h.opts |= kOptCookie;
                    break;
                case 2:
                    if (len != 2) return std::nullopt;
                    h.mss = r16();
                    h.opts |= kOptMss;
                    break;
                case 5:
                    if (len & 3) return std::nullopt;
                    h.nsack = std::min<u32>(len >> 2, 8);
                    for (u32 i = 0; i < len / 4u; i++) {
                        const u16 a = r16(), b = r16();
                        if (i < 8) h.sack[i] = {a, b};
                    }
                    break;
                case 10:
                    if (len != 4) return std::nullopt;
                    h.retransmission = r32();
                    h.ext |= kExtRetransmission;
                    break;
                case 11:
                    if (len != 4) return std::nullopt;
                    h.stamp = r32();
                    h.ext |= kExtTimestamp;
                    break;
                default: at += len; break;
                }
            }
            if (final) break;
        }
    }
    return at;
}

} // namespace rudp

namespace {

using rudp::Header;

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
constexpr s32 err(u32 code) { return static_cast<s32>(code); }

// A context's state as CellRudpContextStatus and the connection have it.
enum : u32 {
    kIdle = 0,
    kClosed = 1, // also an Activate waiting for its peer's SYN
    kSynSent = 2,
    kSynRcvd = 3,
    kEstablished = 4,
    kCloseWait = 5,
    kListening = 6, // the API's only
};

// Why a connection ended (the RST's reason byte; the API's error codes in this order).
enum : u8 {
    kWhyNone = 0,
    kWhyReset = 1,
    kWhyRefused = 2,
    kWhyTimeout = 3,
    kWhyVersion = 4,
    kWhyTransport = 5,
    kWhyQuality = 6,
    kWhyInternal = 7,
    kWhyKeepAlive = 8,
};
u32 error_of(u8 why)
{
    switch (why) {
    case kWhyNone: return 0;
    case kWhyReset: return CELL_RUDP_ERROR_CONN_RESET;
    case kWhyRefused: return CELL_RUDP_ERROR_CONN_REFUSED;
    case kWhyTimeout: return CELL_RUDP_ERROR_CONN_TIMEOUT;
    case kWhyVersion: return CELL_RUDP_ERROR_CONN_VERSION_MISMATCH;
    case kWhyTransport: return CELL_RUDP_ERROR_CONN_TRANSPORT_TYPE_MISMATCH;
    case kWhyQuality: return CELL_RUDP_ERROR_QUALITY_LEVEL_MISMATCH;
    case kWhyKeepAlive: return CELL_RUDP_ERROR_KEEP_ALIVE_FAILURE;
    default: return CELL_RUDP_ERROR_INTERNAL;
    }
}

// The context handler's events (CellRudpContextEventHandler) and the event handler's.
enum : u32 {
    kEvClosed = 1,
    kEvEstablished = 2,
    kEvError = 3,
    kEvWritable = 4,
    kEvReadable = 5,
    kEvFlushed = 6,
    kEvAcceptable = 7,
};
enum : u32 { kEvSend = 1, kEvSocketReleased = 2, kEvBeforeSend = 100, kEvBeforeReceive = 101 };

// Polling (RPCS3 cellRudp.h).
enum : u16 { kPollRead = 0x0001, kPollWrite = 0x0002, kPollFlush = 0x0004, kPollError = 0x0008 };

// Write flags.
enum : u8 {
    kMsgDontwait = 0x01,
    kMsgLatencyCritical = 0x08,
    kMsgAlign32 = 0x10,
    kMsgAlign64 = 0x20,
    kMsgWithTimestamp = 0x40,
};

// The connection's flags.
enum : u32 {
    kCInitiated = 0x1,     // Initiate's handshake runs
    kCWantWritable = 0x4,  // a write found no room: WRITABLE when there is
    kCFlushing = 0x8,      // a Flush waits for its marked segment to be acknowledged
    kCSendQueued = 0x10,   // an output event is queued
    kCAckQueued = 0x20,    // an acknowledgement event is queued
    kCActivated = 0x80,    // Activate waits for the peer's SYN
    kCOrder = 0x10000,     // CELL_RUDP_OPTION_ORDER_CRITICAL
    kCDelivery = 0x20000,  // CELL_RUDP_OPTION_DELIVERY_CRITICAL
    kCStream = 0x40000,    // CELL_RUDP_OPTION_STREAM
    kCNodelay = 0x80000,   // CELL_RUDP_OPTION_NODELAY
    kCSack = 0x100000,     // option 12: selective acknowledgements
    kCFreshIds = 0x400000, // the next handshake takes new token and sequence numbers
    kCOpt255 = 0x80000000, // option 0xFF
};
constexpr u32 kCDefault = kCOrder | kCDelivery | kCSack | 0x200000 | kCFreshIds;

// The context's flags.
enum : u8 { kXListening = 0x1, kXPeer = 0x2, kXTerminated = 0x4 };
// What a blocked call waits for (bits set by the events, cleared by the waiter; 0x80: the context goes).
enum : u8 {
    kWakeEstablished = 0x1,
    kWakeClosed = 0x2,
    kWakeWritable = 0x4,
    kWakeReadable = 0x8,
    kWakeFlushed = 0x10,
    kWakeAcceptable = 0x20,
    kWakeCancel = 0x80,
};

constexpr u16 kDefaultMss = 0x582; // 1410: header, payload and the datagram's prefix, one segment
constexpr u32 kPayloadSlack = 0x40; // what a segment leaves for the header: MAX_PAYLOAD is the segment less this
constexpr u64 kMinRto = 1'000'000, kMaxRto = 16'000'000, kInitialRto = 1'000'000; // microseconds
constexpr u64 kMinVariance = 16'000;
constexpr u32 kIoPollUs = 16'667;    // the internal I/O thread's socketselect timeout
constexpr u32 kIoBufferDefault = 0x800;
constexpr u32 kSysNetSoNbio = 0x1100, kSysNetSolSocket = 0xFFFF;

// librudp's clock: sys_time_get_current_time in microseconds (the wall clock; its time stamps are its milliseconds).
u64 now_us()
{
    s64 sec = 0, nsec = 0;
    lv2::host_current_time(sec, nsec);
    return static_cast<u64>(sec) * 1000000 + static_cast<u64>(nsec) / 1000;
}
u32 now_ms32() { return static_cast<u32>(now_us() / 1000); }
s16 sdiff(u16 a, u16 b) { return static_cast<s16>(static_cast<u16>(a - b)); }
u64 sext(s32 v) { return static_cast<u64>(static_cast<s64>(v)); }

// ------------------------------------------------------------------------------------------------- the lock

// librudp's lock: a recursive lightweight mutex. A blocked call releases it entirely while it waits (as an lwcond
// wait does) and takes it back as deep as it was.
class LibLock {
public:
    void lock()
    {
        const std::thread::id me = std::this_thread::get_id();
        std::unique_lock l(m_);
        if (owner_ == me) {
            depth_++;
            return;
        }
        cv_.wait(l, [&] { return depth_ == 0; });
        owner_ = me;
        depth_ = 1;
    }
    void unlock()
    {
        std::unique_lock l(m_);
        if (--depth_ == 0) {
            owner_ = {};
            cv_.notify_one();
        }
    }
    u32 release_all()
    {
        std::unique_lock l(m_);
        const u32 d = depth_;
        depth_ = 0;
        owner_ = {};
        cv_.notify_one();
        return d;
    }
    void reacquire(u32 d)
    {
        const std::thread::id me = std::this_thread::get_id();
        std::unique_lock l(m_);
        cv_.wait(l, [&] { return depth_ == 0; });
        owner_ = me;
        depth_ = d;
    }
    bool held() const
    {
        std::unique_lock l(m_);
        return depth_ && owner_ == std::this_thread::get_id();
    }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread::id owner_{};
    u32 depth_ = 0;
};

// A condition variable over the library's lock (librudp's lwconds: a context's, a poll set's, the scheduler's).
// Blocked guest threads park on lv2 waiters, show as network waits, and the process's end unwinds them.
class Cond {
public:
    // Waits for signal() or signal_all() or `timeout` microseconds (0: none). The lock is held on entry and on
    // return. True when signalled.
    bool wait(LibLock& lock, lv2::PpuThread& t, u64 timeout);
    void signal_all();
    void signal();

private:
    std::mutex m_;
    lv2::WaitQueue q_;
};

bool Cond::wait(LibLock& lock, lv2::PpuThread& t, u64 timeout)
{
    std::unique_lock lk(m_);
    lv2::Waiter& w = t.waiter;
    lv2::prepare_wait(w);
    q_.push(&w);
    const u32 depth = lock.release_all();
    const lv2::WaitStatus status = lv2::park(w, lk, timeout, {lv2::WaitKind::Net, 0}, [&](bool) {
        return q_.remove(&w) ? lv2::CancelResult::Removed : lv2::CancelResult::Woken;
    });
    if (status == lv2::WaitStatus::Aborted) {
        lock.reacquire(depth);
        throw lv2::ThreadExit{};
    }
    lock.reacquire(depth);
    return status == lv2::WaitStatus::Woken;
}

void Cond::signal_all()
{
    std::lock_guard lk(m_);
    for (lv2::Waiter* w : q_.take_all(lv2::SYS_SYNC_FIFO)) lv2::wake(*w, lv2::CELL_OK);
}

void Cond::signal()
{
    std::lock_guard lk(m_);
    if (lv2::Waiter* w = q_.pop(lv2::SYS_SYNC_FIFO)) lv2::wake(*w, lv2::CELL_OK);
}

// ------------------------------------------------------------------------------------------------- the state

struct Lib;
struct Ctx;
struct Net;
struct Seg;

// The PPU thread calling into the library right now (an API call, the I/O thread): guest functions (the allocator,
// the handlers) are called on it.
thread_local PPUContext* t_ppu = nullptr;
struct OnThread {
    PPUContext* saved;
    explicit OnThread(PPUContext& ctx) : saved(t_ppu) { t_ppu = &ctx; }
    ~OnThread() { t_ppu = saved; }
    OnThread(const OnThread&) = delete;
    OnThread& operator=(const OnThread&) = delete;
};

// One allocation the library made through its allocator (librudp's: 16 bytes of its own before each block, counted
// in CellRudpStatus's allocs, frees, memCurrent and memPeak). `addr` is the game's block (with the library's own
// allocator, nothing is taken from the game's heap: 1 marks the block as held).
struct Block {
    u32 addr = 0, size = 0;
    explicit operator bool() const { return size != 0; }
};

// A segment: a packet's payload to send or one received, from the connection's pool (librudp keeps freed ones for
// the next).
struct Seg {
    std::vector<u8> buf; // the payload's room (its size: the pool's when the segment was made)
    u32 off = 0, len = 0; // what is in it: buf[off, off + len)
    u32 stamp = 0;        // to send: the millisecond clock at its first sending; received: the sender's time stamp
    u32 retrans = 0;      // to send: retransmissions (bits 31-24) and their delay in ms (23-0); received: as sent
    u16 flags = 0;
    u16 seq = 0;
    Seg* next = nullptr;
    Block mem_obj, mem_buf;
    u32 room() const { return static_cast<u32>(buf.size()) - off - len; }
};
enum : u16 {
    kSegSent = 0x1,     // to send: gone at least once
    kSegLost = 0x2,     // to send: to send again; received: delivered in order (a stream's bytes)
    kSegSacked = 0x4,   // to send: the peer has it (a selective acknowledgement); received: the oldest latency-critical
    kSegFlush = 0x8,    // the last segment before a Flush
    kSegAlign32 = 0x10,
    kSegAlign64 = 0x20,
    kSegLatency = 0x100, // latency critical
    kSegMore = 0x200,    // a stream segment followed by more of the same write
    kSegStamp = 0x800,   // carries a time stamp
    kSegSend = 0x8000,   // a segment to send (a received one otherwise)
};

struct Pool {
    Seg* free = nullptr;
    u16 nfree = 0, total = 0, nsend = 0, nrecv = 0, max_send = 0, max_recv = 0;
    u16 bufsize = static_cast<u16>(kDefaultMss - kPayloadSlack); // MAX_PAYLOAD
};

// A connection (one per context): the protocol's state.
struct Conn {
    Ctx* owner = nullptr;
    u32 token = 0, peer_token = 0;
    u32 segsize = kDefaultMss;          // MAX_PAYLOAD + 64, the peer's segment size once connected
    u32 sndbuf = 0x10000, rcvbuf = 0x10000;
    Pool pool;
    u16 snd_una = 0, snd_nxt = 0;       // the oldest unacknowledged sequence number, the next to give
    u16 peer_window = 0;                // the peer's receive slots (its SYN)
    u16 window = 0;                     // the peer's receive slots (its last packet)
    u32 cwnd = kDefaultMss, ssthresh = 0x10000;
    s32 recover = -1;                   // the sequence number fast recovery ends at
    u16 recover_seq = 0;
    u16 rcv_nxt = 0, rcv_high = 0, rcv_low = 0, rcv_last = 0;
    Seg *sq_head = nullptr, *sq_tail = nullptr; // the segments to send and those in flight, in order
    u32 sq_bytes = 0, sq_unsent = 0;    // their payload; what of it is still to send
    Seg *rq_head = nullptr, *rq_tail = nullptr, *rq_ordered = nullptr; // received, waiting to be read
    u32 rq_bytes = 0, rq_ordered_bytes = 0;
    u16 rq_latency = 0;                 // latency-critical messages waiting (order not critical)
    u32 readable = 0;                   // what the next Read gives (a message's size)
    Seg* rq_next_latency = nullptr;
    u16 dupacks = 0, backoff = 0, delayed_acks = 0;
    u32 conn_timeout = 60000, close_wait_timeout = 0, aggregation = 30; // milliseconds
    u64 rttvar = 0, srtt = 0, rto = kInitialRto, rexmit_at = 0, deadline = 0, min_var = kMinVariance;
    u64 last_data = 0, delack_since = 0, persist_since = 0;
    u32 persist_ms = 0;
    u32 ka_interval = 0, ka_timeout = 20000; // milliseconds
    u64 ka_first = 0, ka_last = 0, ka_heard = 0, ka_next = 0;
    u32 ka_count = 0;
    u64 last_rcvd = 0, last_sent = 0;
    std::vector<u8> cookie; // a listener's cookie to echo in the SYN
    Block cookie_mem;
    u32 state = kIdle;
    u32 flags = kCDefault;
    // CellRudpContextStatus.
    u32 lost = 0, recovered = 0, dups = 0, sent_packets = 0, rcvd_packets = 0, retransmissions = 0;
    u64 sent_bytes = 0, rcvd_bytes = 0;

    bool stream() const { return flags & kCStream; }
    bool delivery() const { return flags & kCDelivery; }
    bool order() const { return flags & kCOrder; }
    bool reliable() const { return stream() || delivery(); }
    u32 payload() const { return segsize - kPayloadSlack; }
    u32 slots(u32 buffer) const { return segsize <= kPayloadSlack ? buffer : buffer / (segsize - kPayloadSlack); }
    u16 free_rcv_slots() const
    {
        const u16 n = static_cast<u16>(segsize <= kPayloadSlack ? static_cast<u16>(rcvbuf) : rcvbuf / payload());
        return n > pool.nrecv ? static_cast<u16>(n - pool.nrecv) : 0;
    }
    s32 writable() const;
    s32 readable_size() const;
};

// The scheduler's event: for the object registered as `target`.
struct Event {
    u32 target = 0;
    u8 type = 0, sub = 0;
    u32 arg = 0;
};
// A context's events (librudp's types; what they do is Ctx::on_event).
enum : u8 {
    kQEstablished = 0,
    kQClosed = 1,
    kQError = 2,
    kQReadable = 3,
    kQWritable = 4,
    kQFlushed = 5,
    kQSendSyn = 6,
    kQSendAck = 7,
    kQSendRst = 8,
    kQOutput = 9,
    kQProbeReply = 10,
    kQChildEstablished = 11,
    kQChildClosed = 12,
    kQCleanup = 13,
    kQCookies = 14,
    kQMemory = 15,
    kQBadSocket = 16,
    kQSocketReleased = 17, // the networker's
};

struct Target {
    enum class Kind { Ctx, Net, Agg } kind = Kind::Net;
    Ctx* ctx = nullptr;
    u64 interval = 0; // a timer's period in microseconds (0: none)
    Block mem;
};

struct Sched {
    std::deque<Event> ring;
    u32 cap = 0x100;
    Block ring_mem, table_mem;
    std::map<u32, Target> targets;
    struct TimerNode {
        u32 id;
        Block mem;
    };
    std::multimap<u64, TimerNode> timers;
    u32 last_id = 0xFFFFFFFF;
    s32 busy = 0;
    bool cancelled = false, kicked = false;
    Cond cond;
};

// A peer's address as a context keeps it: a sys_net_sockaddr (16 bytes used of 34) and a vport.
struct Addr {
    std::array<u8, 0x22> raw{};
    u16 vport = 0;
};
// librudp's order of addresses: by family, then (IPv4) port and address; else by length and bytes.
bool addr_less(const std::array<u8, 0x22>& a, const std::array<u8, 0x22>& b)
{
    if (a[1] != b[1]) return a[1] < b[1];
    if (a[1] == 2) {
        if (const int c = std::memcmp(&a[2], &b[2], 2)) return c < 0;
        return std::memcmp(&a[4], &b[4], 4) < 0;
    }
    if (!a[0] || a[0] != b[0]) return a[0] && a[0] < b[0];
    return std::memcmp(&a[2], &b[2], static_cast<usize>(a[0] - 2) & 0x1F) < 0;
}
bool addr_equal(const std::array<u8, 0x22>& a, const std::array<u8, 0x22>& b)
{
    if (a[1] != b[1]) return false;
    if (a[1] == 2) return !std::memcmp(&a[2], &b[2], 6);
    if (!a[0]) return true;
    return a[0] == b[0] && !std::memcmp(&a[2], &b[2], static_cast<usize>(b[0] - 2) & 0x1F);
}
struct AddrLess {
    bool operator()(const Addr& a, const Addr& b) const
    {
        if (addr_less(a.raw, b.raw)) return true;
        return addr_equal(a.raw, b.raw) && a.vport < b.vport;
    }
};

// A context.
struct Ctx {
    Net* net = nullptr;
    s32 id = -1;
    u32 target = 0; // its scheduler registration (events and the timer)
    u32 handler = 0, arg = 0;
    Conn c;
    Cond cond;
    bool blocking = false; // CELL_RUDP_OPTION_NONBLOCK 0
    s32 waiters = 0;
    s32 soc = -1;
    std::array<u8, 0x22> local{}; // never set (librudp hashes it into the token)
    u16 vport = 0;
    Addr peer;
    Ctx* parent = nullptr;
    bool child = false;
    struct ChildNode {
        Ctx* ctx;
        Block mem;
    };
    std::multimap<Addr, ChildNode, AddrLess> accepted; // a listener's accepted children
    struct Pending {
        u64 since;
        Ctx* ctx;
        Block mem;
    };
    std::list<Pending> pending; // a listener's children not accepted yet
    u16 backlog = 0;
    u32 cookie_life = 10; // option 13, seconds (0: no cookies)
    struct CookieRing {
        std::array<Addr, 64> entries{};
        u32 head = 0, tail = 0;
        bool full = false;
        Block mem_obj, mem_buf;
    };
    std::unique_ptr<CookieRing> cookies;
    u32 secret = 0;
    u8 mux = 0;
    s32 poll = -1;
    s32 error = 0;
    u32 read_timeout = 0, write_timeout = 0, flush_timeout = 0; // milliseconds
    u8 flags = 0;
    u8 wake = 0;
    bool valid = true;
    Block mem;
};

// What the library keeps of one socket: its contexts, and how arriving packets find them.
struct SockRec {
    s32 soc = -1;
    u16 next_vport = 10000;
    struct CtxNode {
        Ctx* ctx;
        Block mem;
    };
    std::multimap<Ctx*, CtxNode> ctxs;
    struct PeerNode {
        Ctx* ctx;
        Block mem;
    };
    std::multimap<Addr, PeerNode, AddrLess> peers; // mux 1: by the peer's address and the shared vport
    struct VportNode {
        Ctx* ctx;
        Block mem;
    };
    std::multimap<u16, VportNode> vports; // mux 2: by vport
    Block mem;
};

// The aggregator: one datagram per destination at a time, sent when it is full, when a packet that may not wait is
// added, or when its oldest packet has waited its aggregation timeout.
struct AggKey {
    s32 soc = -1;
    std::array<u8, 0x22> addr{};
};
struct AggKeyLess {
    bool operator()(const AggKey& a, const AggKey& b) const
    {
        if (a.soc != b.soc) return a.soc < b.soc;
        return addr_less(a.addr, b.addr);
    }
};
struct AggUser {
    s32 ctx = 0;
    u32 aggregation = 0;
    Block mem;
};
struct AggPacket {
    u32 queued_ms = 0;
    Seg* stamp = nullptr; // the segment whose first-sending time is the datagram's
};
struct AggDest {
    std::vector<u8> bytes; // the datagram so far
    u32 cap = 0;
    u64 start = 0;
    u32 max_wait = 0, wait = 0; // milliseconds
    std::vector<AggPacket> packets;
    std::list<AggUser> users;
    Block mem_obj, mem_buf, mem_node;
};
struct Agg {
    u16 mss = kDefaultMss;
    u32 target = 0;
    u32 interval_ms = 15;
    bool armed = false;
    std::multimap<AggKey, AggDest, AggKeyLess> dests;
};

// A poll set.
struct PollSet {
    s32 id = 0;
    struct Entry {
        u16 req = 0, ret = 0; // ret bit 15: queued in the ring
        Block mem;
    };
    std::multimap<s32, Entry> entries;
    std::vector<s32> ring;
    u32 head = 0, tail = 0;
    bool full = false;
    u32 carried = 0; // entries the last wait handed out, looked at again by the next
    Cond cond;
    s32 waiters = 0, cancelling = 0;
    bool cancel = false, dying = false;
    Block mem_obj, mem_ring, mem_node;
    u32 count() const
    {
        if (ring.empty()) return 0;
        if (tail == head) return full ? static_cast<u32>(ring.size()) : 0;
        return tail > head ? tail - head : static_cast<u32>(ring.size()) + tail - head;
    }
    bool push(s32 v)
    {
        if (ring.empty() || (tail == head && full)) return false;
        full = true;
        ring[tail] = v;
        if (++tail == ring.size()) tail = 0;
        return true;
    }
};

// The internal I/O thread.
struct IoThread {
    bool running = false;   // librudp's thread object's "run" flag
    bool started = false;
    u32 stack = 0x1000, priority = 0x3E9;
    u16 bufsize = kIoBufferDefault;
    Block buf_mem;
    u32 thread_id = 0;
    s32 wake_soc = -1;
    std::array<u8, 16> wake_addr{};
    std::atomic<u32> woken{0};
    std::vector<s32> sockets; // what it reads, the wake-up socket first
    std::atomic<bool> stop{false};
};

struct Net {
    Sched sched;
    std::multimap<s32, std::pair<Ctx*, Block>> ctxs;
    s32 next_ctx = 0;
    u32 handler = 0, handler_arg = 0;
    Agg agg;
    IoThread io;
    bool io_enabled = false;
    std::multimap<s32, std::pair<std::unique_ptr<PollSet>, Block>> polls;
    s32 next_poll = 0;
    std::multimap<s32, std::unique_ptr<SockRec>> sockets;
    u32 target = 0;
    Block mem;
};

struct Lib {
    LibLock lock;
    bool initialized = false, ending = false;
    // Memory.
    u32 app_malloc = 0, app_free = 0;
    u32 mem_current = 0, mem_peak = 0, mem_allocs = 0, mem_frees = 0;
    std::array<u8, 0x128> status{};
    Block status_mem;
    std::unique_ptr<Net> net;
    std::mt19937 rng{std::random_device{}()};
    std::mutex filter_m;
    std::atomic<bool> has_filter{false};
    rudp::SendFilter filter;
};

Lib& lib_of() { return HleProcess::current()->state<Lib>(); }

// ------------------------------------------------------------------------------------------------- memory, counters

Block mem_alloc(Lib& lib, u32 size)
{
    const u32 total = size + 0x10;
    Block b;
    if (lib.app_malloc) {
        if (!t_ppu) PS_FATAL(kChan, "an allocation with no PPU thread to call the game's allocator on");
        const u64 p = call_guest(*t_ppu, lib.app_malloc, {total});
        if (!static_cast<u32>(p)) return {};
        b.addr = static_cast<u32>(p);
    } else {
        b.addr = 1;
    }
    b.size = total;
    lib.mem_allocs++;
    lib.mem_current += total;
    lib.mem_peak = std::max(lib.mem_peak, lib.mem_current);
    return b;
}

void mem_free(Lib& lib, Block& b)
{
    if (!b) return;
    lib.mem_frees++;
    lib.mem_current -= b.size;
    if (lib.app_free && b.addr) {
        if (!t_ppu) PS_FATAL(kChan, "a free with no PPU thread to call the game's allocator on");
        call_guest(*t_ppu, lib.app_free, {b.addr});
    }
    b = {};
}

// CellRudpStatus's counters (librudp's layout).
enum : u32 {
    kStSentUdpPackets = 0x00,
    kStSentUdpBytes = 0x08,
    kStRcvdUdpPackets = 0x10,
    kStRcvdUdpBytes = 0x18,
    kStSentUserPackets = 0x20,
    kStSentUserBytes = 0x28,
    kStSentLatencyCritical = 0x30,
    kStRcvdUserPackets = 0x34,
    kStRcvdUserBytes = 0x38,
    kStRcvdLatencyCritical = 0x40, // reads that took a latency critical message
    kStSentSyn = 0x44,
    kStRcvdSyn = 0x48,
    kStSentUsr = 0x4C,
    kStRcvdUsr = 0x50,
    kStSentPrb = 0x54,
    kStRcvdPrb = 0x58,
    kStSentRst = 0x5C,
    kStRcvdRst = 0x60,
    kStLost = 0x64,
    kStRetransmitted = 0x68,
    kStReordered = 0x6C,
    kStLevels = 0x70, // per quality level: u32 sent packets, u64 sent bytes, u32 received packets, u64 received bytes
    kStContexts = 0xF0,
    kStAllocs = 0xF4,
    kStFrees = 0xF8,
    kStMemCurrent = 0xFC,
    kStMemPeak = 0x100,
    kStEstablished = 0x104,
    kStFailed = 0x108,
    kStFailedReset = 0x10C,
    kStFailedRefused = 0x110,
    kStFailedTimeout = 0x114,
    kStFailedVersion = 0x118,
    kStFailedTransport = 0x11C,
    kStFailedQuality = 0x120,
};

void st_add32(Lib& lib, u32 off, s32 v) { ps_st32(lib.status.data() + off, ps_ld32(lib.status.data() + off) + v); }
void st_add64(Lib& lib, u32 off, u64 v) { ps_st64(lib.status.data() + off, ps_ld64(lib.status.data() + off) + v); }

// A packet's type counted as sent (or received).
void st_type(Lib& lib, u8 type, bool sent)
{
    static constexpr u32 kSent[4] = {kStSentUsr, kStSentPrb, kStSentSyn, kStSentRst};
    static constexpr u32 kRcvd[4] = {kStRcvdUsr, kStRcvdPrb, kStRcvdSyn, kStRcvdRst};
    st_add32(lib, sent ? kSent[type & 3] : kRcvd[type & 3], 1);
}

// A connection's quality level (0: delivery and order critical, 1: delivery, 2: order, 3: neither).
u32 level_of(const Conn& c) { return (c.order() ? 0u : 1u) + (c.delivery() ? 0u : 2u); }

// ------------------------------------------------------------------------------------------------- the scheduler

// Registers an object for events and a timer: its ID (0 when there is no room or memory).
s32 sched_register(Lib& lib, Sched& s, Target t, u32& id)
{
    id = 0;
    if (!s.table_mem) return err(CELL_RUDP_ERROR_MEMORY);
    u32 cand = s.last_id + 1;
    if (!cand) cand = 1;
    while (cand != s.last_id) {
        if (!s.targets.count(cand)) {
            t.mem = mem_alloc(lib, 0xC);
            if (!t.mem) return err(CELL_RUDP_ERROR_MEMORY);
            s.targets.emplace(cand, t);
            s.last_id = cand;
            id = cand;
            return 0;
        }
        if (!++cand) cand = 1;
    }
    return err(CELL_RUDP_ERROR_INTERNAL);
}

void sched_unregister(Lib& lib, Sched& s, u32 id)
{
    const auto it = s.targets.find(id);
    if (it == s.targets.end()) return;
    mem_free(lib, it->second.mem);
    s.targets.erase(it);
}

// Starts (or restarts) a target's timer: every `interval` microseconds from now. Each start adds a timer entry.
s32 sched_arm(Lib& lib, Sched& s, u32 id, u64 interval)
{
    if (!s.table_mem) return err(CELL_RUDP_ERROR_MEMORY);
    const auto it = s.targets.find(id);
    if (it == s.targets.end()) return err(CELL_RUDP_ERROR_INTERNAL);
    it->second.interval = interval;
    Block node = mem_alloc(lib, 0x28);
    if (!node) return err(CELL_RUDP_ERROR_MEMORY);
    s.timers.emplace(now_us() + interval, Sched::TimerNode{id, node});
    return 0;
}

// Stops a target's timer (its entries go when they come due).
void sched_disarm(Sched& s, u32 id)
{
    const auto it = s.targets.find(id);
    if (it != s.targets.end()) it->second.interval = 0;
}

void io_wake(Net& net);

// Queues an event; the scheduler's waiter and the I/O thread wake.
s32 sched_post(Lib& lib, Net& net, Event e)
{
    Sched& s = net.sched;
    if (s.ring.size() == s.cap && s.cap) {
        if (s.cap > 0xFFFF) return err(CELL_RUDP_ERROR_INTERNAL);
        Block grown = mem_alloc(lib, s.cap * 2 * 12);
        if (!grown) return err(CELL_RUDP_ERROR_MEMORY);
        mem_free(lib, s.ring_mem);
        s.ring_mem = grown;
        s.cap *= 2;
    }
    if (!s.ring_mem || !s.cap || s.ring.size() >= s.cap) return err(CELL_RUDP_ERROR_INTERNAL);
    s.ring.push_back(e);
    s.kicked = true;
    s.cond.signal();
    io_wake(net);
    return 0;
}

void ctx_on_event(Lib& lib, Ctx& x, const Event& e);
void ctx_on_timer(Lib& lib, Ctx& x, u64& interval);
void net_on_event(Lib& lib, Net& net, const Event& e);
void agg_on_timer(Lib& lib, Net& net, u64& interval);

// Runs what is due: the timers, then the events (those they add too); with a timeout (microseconds), then waits up to
// that long for more, and runs it. CANCELLED when cellRudpEnd ends it.
s32 sched_run(Lib& lib, Net& net, u64 timeout)
{
    Sched& s = net.sched;
    lv2::PpuThread& t = thread_of(*t_ppu);
    s.busy++;
    s32 result = 0;
    if (!s.cancelled) {
        if (!s.table_mem) {
            result = err(CELL_RUDP_ERROR_MEMORY);
        } else {
            const u64 start = now_us();
            for (;;) {
                const u64 now = now_us();
                while (!s.timers.empty() && s.timers.begin()->first <= now) {
                    Sched::TimerNode node = s.timers.begin()->second;
                    s.timers.erase(s.timers.begin());
                    const auto it = s.targets.find(node.id);
                    if (it == s.targets.end() || !it->second.interval) {
                        mem_free(lib, node.mem);
                        continue;
                    }
                    Target& tg = it->second;
                    u64 interval = tg.interval;
                    if (tg.kind == Target::Kind::Ctx && tg.ctx) ctx_on_timer(lib, *tg.ctx, interval);
                    else if (tg.kind == Target::Kind::Agg) agg_on_timer(lib, net, interval);
                    const auto again = s.targets.find(node.id); // the callback may have let the target go
                    if (again == s.targets.end()) {
                        mem_free(lib, node.mem);
                        continue;
                    }
                    again->second.interval = interval;
                    s.timers.emplace(now + interval, node);
                }
                while (!s.ring.empty()) {
                    const Event e = s.ring.front();
                    s.ring.pop_front();
                    const auto it = s.targets.find(e.target);
                    if (it == s.targets.end()) continue;
                    if (it->second.kind == Target::Kind::Ctx && it->second.ctx) ctx_on_event(lib, *it->second.ctx, e);
                    else if (it->second.kind == Target::Kind::Net) net_on_event(lib, net, e);
                }
                if (!timeout) break;
                s.kicked = false;
                bool done = false, more = false;
                while (!s.kicked) {
                    const u64 tn = now_us();
                    const u32 elapsed = static_cast<u32>(tn - start);
                    if (timeout <= u64{elapsed} + 125) {
                        done = true;
                        break;
                    }
                    if (!s.ring.empty()) {
                        more = true;
                        break;
                    }
                    u64 wait = timeout - elapsed;
                    bool overall = elapsed != 0;
                    if (!s.timers.empty()) {
                        const u64 due = s.timers.begin()->first;
                        if (due <= tn) {
                            more = true;
                            break;
                        }
                        if (due - tn < wait) {
                            overall = false;
                            wait = due - tn;
                        }
                    }
                    const bool woken = s.cond.wait(lib.lock, t, wait ? wait : 1);
                    if (!woken && overall) {
                        done = true;
                        break;
                    }
                    if (s.cancelled) {
                        done = true;
                        break;
                    }
                }
                if (done && !more) break;
            }
        }
    }
    s.busy--;
    if (s.cancelled) {
        result = err(CELL_RUDP_ERROR_CANCELLED);
        if (!s.busy) {
            s.cond.signal_all();
            s.cancelled = false;
        }
    }
    return result;
}

// ------------------------------------------------------------------------------------------------- segments

Seg* pool_get(Lib& lib, Conn& c, bool send)
{
    Pool& p = c.pool;
    Seg* s = p.free;
    if (!s) {
        Block obj = mem_alloc(lib, 0x28);
        if (!obj) return nullptr;
        Block buf = mem_alloc(lib, p.bufsize);
        if (!buf) return nullptr; // librudp keeps the first block (lost)
        s = new Seg;
        s->mem_obj = obj;
        s->mem_buf = buf;
        s->buf.assign(p.bufsize, 0);
        p.total++;
    } else {
        p.nfree--;
        p.free = s->next;
    }
    s->seq = 0;
    s->flags = 0;
    s->stamp = 0;
    s->retrans = 0;
    s->next = nullptr;
    s->off = s->len = 0;
    if (send) {
        s->flags = kSegSend;
        if (++p.nsend > p.max_send) p.max_send = p.nsend;
    } else {
        if (++p.nrecv > p.max_recv) p.max_recv = p.nrecv;
    }
    return s;
}

void pool_put(Conn& c, Seg* s)
{
    Pool& p = c.pool;
    s->next = p.free;
    p.nfree++;
    p.free = s;
    if (s->flags & kSegSend) p.nsend--;
    else p.nrecv--;
}

// Frees the segments kept for later (librudp's pool reset).
void pool_clear(Lib& lib, Conn& c)
{
    Pool& p = c.pool;
    while (Seg* s = p.free) {
        p.free = s->next;
        mem_free(lib, s->mem_buf);
        mem_free(lib, s->mem_obj);
        delete s;
    }
    p.nrecv = p.nfree = p.total = p.nsend = 0;
}

void seg_append(Seg& s, const u8* p, u32 n)
{
    if (n > s.room()) return; // librudp's buffer refuses a write that does not fit (and its callers go on)
    std::memcpy(s.buf.data() + s.off + s.len, p, n);
    s.len += n;
}

void seg_consume(Seg& s, u8* out, u32 n)
{
    if (n > s.len) n = s.len;
    if (out) std::memcpy(out, s.buf.data() + s.off, n);
    s.off += n;
    s.len -= n;
}

// A byte buffer as librudp passes data around: room `cap`, `len` bytes from `off`.
struct Bytes {
    u8* p = nullptr;
    u32 off = 0, len = 0, cap = 0;
    u32 room() const { return cap - off - len; }
};

// ------------------------------------------------------------------------------------------------- the connection

void ctx_post(Ctx& x, u8 type, u8 sub = 0, u32 arg = 0);
void ctx_established(Ctx& x);
void ctx_closed(Ctx& x, u8 why);
void ctx_send(Ctx& x, const Header& h, const u8* payload, u32 len, u32 agg_ms, Seg* stamp);
void ctx_drop_queue(Ctx& x);
void ctx_init_params(Ctx& x, u16 (&p)[3]);

void conn_queue_output(Conn& c)
{
    if (!(c.flags & kCSendQueued) && c.owner) {
        c.flags |= kCSendQueued;
        ctx_post(*c.owner, kQOutput);
    }
}

void conn_queue_ack(Conn& c)
{
    if (!(c.flags & kCAckQueued) && c.owner) {
        c.flags |= kCAckQueued;
        ctx_post(*c.owner, kQSendAck);
    }
}

u64 rto_capped(const Conn& c, u32 shift = 0)
{
    const u64 v = shift < 64 ? c.rto << shift : 0;
    return (shift >= 64 || v >= kMaxRto || (shift && (v >> shift) != c.rto)) ? kMaxRto : v;
}

// The bytes a write may take now (CELL_RUDP_ERROR_NOT_ACCEPTABLE before a connection).
s32 Conn::writable() const
{
    if (state != kEstablished && state != kCloseWait) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    if (state != kEstablished) return 0;
    const u32 n = slots(sndbuf);
    if (n <= pool.nsend || !(n - pool.nsend)) return 0;
    if (!stream() && (!delivery() || !order())) return static_cast<s32>(payload());
    return static_cast<s32>(payload() * (n - pool.nsend));
}

// What a read would give: a message's size, a stream's bytes; END_OF_DATA once the peer has ended and all is read.
s32 Conn::readable_size() const
{
    if (state != kEstablished && state != kCloseWait) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    const s32 n = static_cast<s32>(stream() ? rq_ordered_bytes : readable);
    if (!n && state != kEstablished) return err(CELL_RUDP_ERROR_END_OF_DATA);
    return n;
}

// Everything queued goes; the congestion state starts again.
void conn_drop_send(Conn& c)
{
    if (c.sq_head && c.owner) ctx_drop_queue(*c.owner);
    while (Seg* s = c.sq_head) {
        c.sq_head = s->next;
        pool_put(c, s);
    }
    c.cwnd = kDefaultMss;
    c.ssthresh = 0x10000;
    c.recover = -1;
    c.recover_seq = 0;
    c.dupacks = 0;
    c.snd_una = 0;
    c.peer_window = 0;
    c.window = 0;
    c.sq_head = c.sq_tail = nullptr;
    c.sq_bytes = c.sq_unsent = 0;
}

void conn_reset(Lib& lib, Conn& c)
{
    conn_drop_send(c);
    while (Seg* s = c.rq_head) {
        c.rq_head = s->next;
        pool_put(c, s);
    }
    c.readable = 0;
    c.rq_head = c.rq_tail = c.rq_ordered = nullptr;
    c.rq_bytes = c.rq_ordered_bytes = 0;
    c.rq_latency = 0;
    c.rcv_last = c.rcv_nxt = c.rcv_high = 0;
    pool_clear(lib, c);
}

void conn_free_cookie(Lib& lib, Conn& c)
{
    if (c.cookie_mem) mem_free(lib, c.cookie_mem);
    c.cookie.clear();
}

// The state a new handshake (or a failed one) starts from.
void conn_restart(Lib& lib, Conn& c, u32 keep_flags_mask)
{
    conn_reset(lib, c);
    conn_free_cookie(lib, c);
    c.backoff = 0;
    c.rto = kInitialRto;
    c.min_var = kMinVariance;
    c.last_sent = 0;
    c.flags &= keep_flags_mask;
    c.peer_token = 0;
    c.rttvar = c.srtt = c.rexmit_at = c.deadline = 0;
    c.delack_since = c.persist_since = 0;
    c.persist_ms = 0;
    c.ka_first = c.ka_last = c.ka_heard = c.ka_next = 0;
    c.ka_count = 0;
    c.last_rcvd = 0;
}

// The token and first sequence number of a new handshake (fresh ones the first time, the token's count after).
void conn_take_ids(Conn& c)
{
    u16 p[3] = {0, 0, 0};
    if (c.owner) ctx_init_params(*c.owner, p);
    if (c.flags & kCFreshIds) {
        c.flags &= ~kCFreshIds;
        c.token = static_cast<u32>(p[0]) << 16 | p[1];
        c.snd_nxt = p[2];
    } else {
        c.token = static_cast<u32>((c.token >> 16) + 1) << 16 | p[1];
    }
    c.snd_una = c.snd_nxt;
}

void conn_arm_handshake(Conn& c)
{
    const u64 now = now_us();
    c.backoff = 0;
    c.deadline = now + u64{c.conn_timeout} * 1000;
    c.rexmit_at = now_us() + rto_capped(c);
}

s32 conn_initiate(Lib& lib, Conn& c)
{
    if (!c.owner || c.state == kEstablished || c.state == kCloseWait) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    if (c.flags & kCActivated) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    if (c.flags & kCInitiated) return err(CELL_RUDP_ERROR_IN_PROGRESS);
    conn_restart(lib, c, 0xFFFFFF00);
    conn_take_ids(c);
    c.state = kSynSent;
    c.flags |= kCInitiated;
    ctx_post(*c.owner, kQSendSyn);
    conn_arm_handshake(c);
    return 0;
}

s32 conn_activate(Lib& lib, Conn& c)
{
    if (!c.owner || c.state == kEstablished || c.state == kCloseWait) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    if (c.flags & kCInitiated) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    if (c.flags & kCActivated) return err(CELL_RUDP_ERROR_IN_PROGRESS);
    conn_restart(lib, c, 0xFFFFFF00);
    conn_take_ids(c);
    c.state = kClosed;
    c.flags |= kCActivated;
    conn_arm_handshake(c);
    return 0;
}

void conn_send_rst(Lib& lib, Conn& c, u32 token, u8 code);

void conn_close_timers(Conn& c)
{
    const u64 now = now_us();
    c.deadline = now + u64{c.close_wait_timeout} * 1000;
    c.rexmit_at = now_us() + rto_capped(c);
    c.flags &= ~(kCInitiated | kCActivated);
}

// The connection ends for `why`: told to the peer with a RST (mode 1: queued, 2: at once; 0: not told).
void conn_abort(Lib& lib, Conn& c, u8 why, u32 mode)
{
    const u32 st = c.state;
    if (st == kIdle || st == kCloseWait) return;
    conn_drop_send(c);
    if (mode) {
        const u8 code = why > 8 ? 0xFF : why == kWhyKeepAlive ? kWhyReset : why;
        if (mode == 1) {
            if (c.owner) ctx_post(*c.owner, kQSendRst, code, c.token);
        } else {
            conn_send_rst(lib, c, c.token, code);
        }
    }
    if (st == kEstablished) {
        c.state = kCloseWait;
        if (why && c.owner) ctx_post(*c.owner, kQError, why);
        conn_close_timers(c);
        return;
    }
    if (why != kWhyKeepAlive) {
        st_add32(lib, kStFailed, 1);
        static constexpr u32 kWhy[9] = {0, kStFailedReset, kStFailedRefused, kStFailedTimeout, kStFailedVersion,
            kStFailedTransport, kStFailedQuality, 0, kStFailedQuality};
        if (why <= 8 && kWhy[why]) st_add32(lib, kWhy[why], 1);
    }
    if (st == kSynSent || st == kSynRcvd) {
        if (c.flags & kCInitiated) {
            c.state = kCloseWait;
            if (why && c.owner) ctx_closed(*c.owner, why);
            conn_close_timers(c);
            return;
        }
        c.state = kClosed;
        conn_restart(lib, c, 0xFFFFFF00);
        c.flags &= ~(kCInitiated | kCActivated);
        return;
    }
    if (c.flags & kCActivated) {
        if (c.owner) ctx_closed(*c.owner, why);
    } else if (c.owner) {
        ctx_post(*c.owner, kQCleanup);
    }
    conn_restart(lib, c, 0xFFFFFF00);
    if (c.state == kCloseWait) conn_close_timers(c);
    else c.flags &= ~(kCInitiated | kCActivated);
}

s32 conn_terminate(Lib& lib, Conn& c)
{
    if (c.state == kIdle || c.state == kClosed) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    if (c.state == kCloseWait) return 0;
    conn_abort(lib, c, kWhyNone, 1);
    return 0;
}

// The round trip measured as `ms` milliseconds (smoothed 88/12, its deviation 75/25; the timeout their sum with four
// deviations, at least the timer's granularity, 1 to 16 s); a sample far off the mean shrinks the window by an eighth.
void conn_rtt(Conn& c, u32 ms)
{
    const u64 sample = u64{ms} * 1000;
    u64 dev = 0;
    if (!c.srtt) {
        c.srtt = sample;
        c.rttvar = sample >> 1;
    } else {
        dev = sample - c.srtt;
        c.srtt = c.srtt * 88 / 100 + u64{ms} * 12000 / 100;
        const s64 d = static_cast<s64>(dev);
        c.rttvar = c.rttvar * 75 / 100 + static_cast<u64>(d < 0 ? -d : d) * 25 / 100;
    }
    c.rto = c.srtt + std::max(c.min_var, c.rttvar * 4);
    c.rto = std::clamp<u64>(c.rto, kMinRto, kMaxRto);
    if (c.rttvar < 1001) return;
    if (static_cast<s64>(dev) <= static_cast<s64>(c.rttvar * 4)) return;
    c.cwnd -= c.cwnd >> 3;
    if (c.cwnd < c.segsize) c.cwnd = c.segsize;
    if (c.ssthresh > c.cwnd) c.ssthresh = c.cwnd;
}

// What can be read changed: the size of the next read, and READABLE when there is something.
void conn_update_readable(Conn& c)
{
    if (c.state != kEstablished && c.state != kCloseWait) {
        c.readable = 0;
        c.flags &= ~0x2u;
        return;
    }
    Seg* s = c.rq_head;
    if (!s) {
        c.readable = 0;
        return;
    }
    if (c.readable) return;
    const u32 f = c.flags;
    const auto pick_latency = [&]() -> bool {
        for (Seg* x = c.rq_head; x; x = x->next) {
            if ((x->flags & kSegLatency) && !(x->flags & kSegSent)) {
                c.rq_next_latency = x;
                x->flags |= kSegSacked;
                c.readable = x->len;
                return true;
            }
        }
        return false;
    };
    bool notify_check = true;
    if (!(f & kCStream)) {
        if (f & kCDelivery) {
            if (!(f & kCOrder)) {
                if (!c.rq_latency) {
                    if (c.rq_ordered_bytes) {
                        for (Seg* x = s; x; x = x->next) {
                            if (!(x->flags & kSegSent)) {
                                c.readable = x->len;
                                break;
                            }
                            if (c.rq_ordered == x) break;
                        }
                    }
                } else {
                    pick_latency();
                }
            } else if (c.rq_ordered_bytes) {
                u32 n = 0;
                for (Seg* x = s; x; x = x->next) {
                    n += x->len;
                    if (!(x->flags & kSegMore)) {
                        c.readable = n;
                        break;
                    }
                    if (c.rq_ordered == x) break;
                }
            }
        } else if (f & kCOrder) {
            c.readable = s->len;
        } else if (c.rq_latency) {
            pick_latency();
        } else {
            c.readable = s->len;
        }
    } else if (c.rq_ordered_bytes) {
        c.readable = c.rq_ordered_bytes;
    }
    if (!notify_check) return;
    const s32 n = static_cast<s32>(f & kCStream ? c.rq_ordered_bytes : c.readable);
    if (n > 0 && !(f & 0x2u) && c.owner) ctx_post(*c.owner, kQReadable);
}

// A received segment into the receive queue.
void conn_receive_segment(Lib& lib, Conn& c, Bytes& payload, const Header& h)
{
    const u32 len = payload.len;
    c.last_data = now_us();
    c.rcv_last = h.seq;
    const u16 gap = static_cast<u16>(h.seq - c.rcv_high);
    if (static_cast<s16>(gap) > 0) {
        if (gap > 1) {
            c.lost += gap - 1u;
            st_add32(lib, kStLost, gap - 1);
        }
        c.rcv_high = h.seq;
    }
    const auto fill = [&](Seg* s) {
        seg_append(*s, payload.p + payload.off, len);
        payload.off += len;
        payload.len = 0;
        s->seq = h.seq;
    };
    const auto marks = [&](Seg* s, bool count_latency) {
        if (h.flags & rudp::kFlagLatency) {
            s->flags |= kSegLatency;
            if (count_latency && !c.order()) c.rq_latency++;
        }
        if (h.flags & rudp::kFlagMore) s->flags |= kSegMore;
        if (h.ext & rudp::kExtRetransmission) s->retrans = h.retransmission;
        if (h.ext & rudp::kExtTimestamp) {
            s->stamp = h.stamp;
            s->flags |= kSegStamp;
        }
    };
    const auto extend = [&](bool account) {
        bool moved = false;
        Seg* o = c.rq_ordered;
        while (o->next && static_cast<s16>(static_cast<u16>(o->seq - o->next->seq + 1)) >= 0) {
            o = o->next;
            c.rq_ordered = o;
            c.rq_ordered_bytes += o->len;
            o->flags |= kSegLost;
            moved = true;
        }
        if (moved && account && c.lost && !(h.ext & rudp::kExtRetransmission)) {
            c.lost--;
            c.recovered++;
            st_add32(lib, kStLost, -1);
            st_add32(lib, kStReordered, 1);
        }
    };
    if (!c.stream() && !c.delivery()) {
        c.rcv_nxt = static_cast<u16>(c.rcv_high + 1);
        if (!c.order()) {
            Seg* s = pool_get(lib, c, false);
            if (!s) return conn_abort(lib, c, 9, 1);
            fill(s);
            if (c.rq_tail) c.rq_tail->next = s;
            else c.rq_head = s;
            c.rq_tail = s;
            marks(s, true);
            c.rq_bytes += len;
            return;
        }
        if (sdiff(h.seq, c.rcv_low) < 0) return;
        Seg *prev = nullptr, *at = c.rq_head;
        while (at) {
            if (at->seq == h.seq) {
                c.dups++;
                return;
            }
            if (sdiff(h.seq, at->seq) < 0) break;
            prev = at;
            at = at->next;
        }
        Seg* s = pool_get(lib, c, false);
        if (!s) return conn_abort(lib, c, 9, 1);
        fill(s);
        marks(s, false);
        c.rq_bytes += len;
        if (at) {
            s->next = at;
            if (c.rq_head == at) c.rq_head = s;
            else prev->next = s;
        } else {
            s->next = nullptr;
            if (prev) c.rq_tail->next = s;
            else c.rq_head = s;
            c.rq_tail = s;
        }
        return;
    }
    if (sdiff(h.seq, c.rcv_nxt) < 0) {
        c.dups++;
        return;
    }
    if (h.seq == c.rcv_nxt) {
        Seg* s = pool_get(lib, c, false);
        if (!s) return conn_abort(lib, c, 9, 1);
        fill(s);
        s->flags |= kSegLost;
        marks(s, true);
        c.rq_ordered_bytes += len;
        c.rcv_nxt++;
        c.rq_bytes += len;
        if (!c.rq_head) {
            s->next = nullptr;
            c.rq_ordered = c.rq_head = c.rq_tail = s;
        } else if (!c.rq_ordered) {
            s->next = c.rq_head;
            c.rq_ordered = c.rq_head = s;
        } else {
            s->next = c.rq_ordered->next;
            c.rq_ordered->next = s;
            if (c.rq_ordered == c.rq_tail) c.rq_tail = s;
            c.rq_ordered = s;
        }
        extend(true);
        c.rcv_nxt = static_cast<u16>(c.rq_ordered->seq + 1);
        return;
    }
    Seg* prev = c.rq_ordered;
    Seg* at = c.rq_ordered ? c.rq_ordered->next : c.rq_head;
    while (at) {
        if (at->seq == h.seq) {
            c.dups++;
            return;
        }
        if (sdiff(h.seq, at->seq) < 0) break;
        prev = at;
        at = at->next;
    }
    Seg* s = pool_get(lib, c, false);
    if (!s) return conn_abort(lib, c, 9, 1);
    fill(s);
    marks(s, true);
    c.rq_bytes += len;
    if (!at) {
        s->next = nullptr;
        if (prev) c.rq_tail->next = s;
        else c.rq_head = s;
        c.rq_tail = s;
        return;
    }
    if (c.rq_head != at) {
        s->next = at;
        if (prev) prev->next = s;
        return;
    }
    s->next = at;
    c.rq_head = s;
    if (sdiff(c.rcv_nxt, s->seq) < 0) {
        c.rq_ordered = nullptr;
        c.rq_ordered_bytes = 0;
        return;
    }
    c.rq_ordered = s;
    c.rq_ordered_bytes = s->len;
    extend(false);
}

void conn_data(Lib& lib, Conn& c, const Header& h, Bytes& payload)
{
    const u16 slots = static_cast<u16>(c.slots(c.rcvbuf));
    if (slots <= c.pool.nrecv && sdiff(h.seq, c.rcv_high) > 0) return;
    conn_receive_segment(lib, c, payload, h);
    if (!c.stream() && !c.delivery()) return conn_update_readable(c);
    if (c.delayed_acks) {
        conn_queue_ack(c);
        return conn_update_readable(c);
    }
    c.delack_since = now_us();
    c.delayed_acks++;
    conn_update_readable(c);
}

void conn_usr(Lib& lib, Conn& c, const Header& h, Bytes& payload)
{
    if (c.state != kEstablished) {
        if (c.state == kCloseWait) {
            if (c.owner) ctx_post(*c.owner, kQSendRst, 1, c.token);
            return;
        }
        if (c.state != kSynRcvd) return;
        if (!(h.flags & rudp::kFlagAck)) {
            conn_queue_ack(c);
        } else {
            c.backoff = 0;
            c.state = kEstablished;
            c.snd_una = c.snd_nxt;
            c.rexmit_at = 0;
            st_add32(lib, kStEstablished, 1);
            if (c.owner) ctx_established(*c.owner);
            if (c.rq_bytes) conn_update_readable(c);
        }
    }
    if (!payload.len) return;
    conn_data(lib, c, h, payload);
}

void conn_mark_lost(Lib& lib, Conn& c, Seg* s)
{
    c.sq_unsent += s->len;
    c.retransmissions++;
    s->flags |= kSegLost;
    st_add32(lib, kStRetransmitted, 1);
    conn_queue_output(c);
}

// An acknowledgement: what it covers leaves the queue (a round trip measured on the way), the window grows or a
// loss is recovered from, WRITABLE when a write found no room before.
s32 conn_ack(Lib& lib, Conn& c, const Header& h)
{
    if (c.state == kSynSent || c.state == kSynRcvd) {
        const u16 expect = static_cast<u16>(c.snd_nxt + 1);
        if (expect != h.ack) {
            conn_abort(lib, c, kWhyRefused, 1);
            return -1;
        }
        c.snd_nxt = expect;
        return 0;
    }
    if (h.type == rudp::kTypeSyn) return 0;
    if (h.type == rudp::kTypePrb) {
        c.ka_heard = now_us();
        return 0;
    }
    if (c.state == kCloseWait || !c.reliable()) return 0;
    const u64 now = now_us();
    Seg* s = c.sq_head;
    if (s && (s->flags & kSegSent)) {
        bool delay = h.flags & rudp::kFlagDelay;
        while (sdiff(s->seq, h.ack) < 0) {
            if (delay && static_cast<u16>(s->seq + 1) == h.ack) {
                const u32 elapsed = static_cast<u32>(now / 1000) - s->stamp;
                if (static_cast<s32>(elapsed) >= 0) conn_rtt(c, elapsed > h.delay ? elapsed - h.delay : 0);
                delay = false;
            }
            if ((c.flags & kCFlushing) && (s->flags & kSegFlush)) {
                c.flags &= ~kCFlushing;
                if (c.owner) ctx_post(*c.owner, kQFlushed);
            }
            c.sq_bytes -= s->len;
            if (s->flags & kSegLost) c.sq_unsent -= s->len;
            c.sq_head = s->next;
            if (!c.sq_head) c.sq_tail = nullptr;
            pool_put(c, s);
            s = c.sq_head;
            if (!s || !(s->flags & kSegSent)) break;
        }
    }
    const u16 prev = c.snd_una;
    c.snd_una = h.ack;
    if (s && sdiff(h.ack, prev) < 1) c.dupacks++;
    bool sacked = false;
    if ((c.flags & kCSack) && h.nsack) {
        const u16 last = h.sack[h.nsack - 1][1];
        Seg* x = c.sq_head;
        while (x && (x->flags & kSegSent) && sdiff(x->seq, last) < 0) {
            for (u32 i = 0; i < h.nsack; i++) {
                if (sdiff(x->seq, h.sack[i][0]) < 0 || sdiff(x->seq, h.sack[i][1]) >= 0) continue;
                sacked = true;
                x->flags |= kSegSacked;
            }
            x = x->next;
        }
    }
    if (c.dupacks < 3) {
        if (sdiff(h.ack, prev) > 0) {
            if (c.cwnd < c.ssthresh) {
                c.cwnd += c.segsize;
            } else {
                const u32 inc = static_cast<u32>((u64{c.segsize} * c.segsize & 0xFFFFFFFFu) / c.cwnd);
                c.cwnd += inc ? inc : 1;
            }
            if (c.cwnd > c.sndbuf) c.cwnd = c.sndbuf;
            if (!c.sq_bytes) {
                if (c.ssthresh < c.cwnd) c.ssthresh = c.cwnd;
                c.cwnd = c.segsize;
            }
        }
    } else {
        Seg* x = nullptr;
        bool walk = true;
        if (c.recover < 0) {
            const u32 half = (c.sq_bytes - c.sq_unsent) >> 1, two = (c.segsize & 0x7FFFFFFF) << 1;
            const u32 v = std::max(half, two);
            c.cwnd = c.ssthresh = v;
            c.recover = c.snd_nxt;
            c.recover_seq = h.ack;
            x = c.sq_head;
            if (!x) walk = false;
            else {
                if (c.reliable() && (x->flags & kSegSent) && !(x->flags & (kSegSacked | kSegLost)))
                    conn_mark_lost(lib, c, x);
                c.recover_seq++;
            }
        } else if (sdiff(h.ack, static_cast<u16>(c.recover)) < 0) {
            x = c.sq_head;
        } else {
            x = c.sq_head;
            c.cwnd = c.ssthresh;
            c.dupacks = 0;
            c.recover = -1;
        }
        if (walk && x && (x->flags & kSegSent)) {
            s32 third = -1, second = -1, newest = -1;
            for (Seg* y = x; y && (y->flags & kSegSent); y = y->next) {
                if (y->flags & kSegSacked) {
                    third = second;
                    second = newest;
                    newest = y->seq;
                }
            }
            if (third >= 0) {
                const u16 lim = static_cast<u16>(third);
                for (Seg* y = c.sq_head; y && (y->flags & kSegSent) && sdiff(y->seq, lim) < 0; y = y->next) {
                    if (!(y->flags & kSegSacked) && sdiff(y->seq, c.recover_seq) >= 0 && c.reliable() &&
                        !(y->flags & kSegLost))
                        conn_mark_lost(lib, c, y);
                }
                c.recover_seq = lim;
            }
        }
    }
    if (sacked || sdiff(h.ack, prev) > 0) {
        c.backoff = 0;
        c.rexmit_at = c.sq_bytes ? now_us() + rto_capped(c) : 0;
    }
    if ((c.flags & kCWantWritable) && c.state == kEstablished && c.writable() > 0) {
        c.flags &= ~kCWantWritable;
        if (c.owner) ctx_post(*c.owner, kQWritable);
    }
    conn_queue_output(c);
    return 0;
}

// The peer's SYN parameters: they must agree with ours (else a RST with the reason, and the error here too).
void conn_syn(Lib& lib, Conn& c, const Header& h)
{
    if (h.lowest > rudp::kVersion || h.version < rudp::kVersion) return conn_abort(lib, c, kWhyVersion, 1);
    if (c.stream()) {
        if (!(h.syn & rudp::kSynStream)) return conn_abort(lib, c, kWhyTransport, 1);
    } else {
        if (h.syn & rudp::kSynStream) return conn_abort(lib, c, kWhyTransport, 1);
        if (c.delivery() != static_cast<bool>(h.syn & rudp::kSynDelivery)) return conn_abort(lib, c, kWhyQuality, 1);
        if (c.order() != static_cast<bool>(h.syn & rudp::kSynOrder)) return conn_abort(lib, c, kWhyQuality, 1);
    }
    const auto take_seq = [&] {
        const u16 seq = static_cast<u16>(h.seq + 1);
        c.rcv_low = c.rcv_nxt = c.rcv_high = c.rcv_last = seq;
        c.peer_window = rudp::decode_window(h.window);
    };
    switch (c.state) {
    case kIdle: return;
    case kClosed: {
        if (h.flags & rudp::kFlagAck) {
            if (c.owner) ctx_post(*c.owner, kQSendRst, 1, c.token);
            return;
        }
        conn_restart(lib, c, ~0x7Fu);
        conn_take_ids(c);
        c.peer_token = h.token;
        c.snd_una = c.snd_nxt;
        c.state = kSynRcvd;
        take_seq();
        const u32 mss = std::max<u32>(h.mss, kPayloadSlack);
        c.segsize = std::min<u32>(mss, (c.pool.bufsize + kPayloadSlack) & 0xFFFF);
        conn_queue_ack(c);
        c.backoff = 0;
        c.deadline = now_us() + u64{c.conn_timeout} * 1000;
        c.rexmit_at = now_us() + rto_capped(c);
        return;
    }
    case kSynSent:
        c.peer_token = h.token;
        take_seq();
        c.segsize = std::min<u32>(std::max<u32>(h.mss, kPayloadSlack), c.segsize);
        conn_queue_ack(c);
        c.backoff = 0;
        if (h.flags & rudp::kFlagAck) {
            c.state = kEstablished;
            c.rexmit_at = 0;
            c.snd_una = c.snd_nxt;
            st_add32(lib, kStEstablished, 1);
            if (c.owner) ctx_established(*c.owner);
        } else {
            c.state = kSynRcvd;
            c.rexmit_at = now_us() + rto_capped(c);
        }
        return;
    case kSynRcvd:
        if (h.token != c.peer_token) return conn_abort(lib, c, kWhyReset, 0);
        if (!(h.flags & rudp::kFlagAck)) {
            conn_queue_ack(c);
            c.rexmit_at = now_us() + rto_capped(c);
            return;
        }
        c.peer_token = h.token;
        c.state = kEstablished;
        take_seq();
        c.segsize = std::clamp<u32>(h.mss, kPayloadSlack, kDefaultMss);
        st_add32(lib, kStEstablished, 1);
        c.backoff = 0;
        c.snd_una = c.snd_nxt;
        c.rexmit_at = 0;
        if (c.owner) ctx_established(*c.owner);
        return;
    case kEstablished:
        if (h.token != c.peer_token) return conn_abort(lib, c, kWhyReset, 0);
        if (h.flags & rudp::kFlagAck) conn_queue_ack(c);
        return;
    case kCloseWait:
        if (c.owner) ctx_post(*c.owner, kQSendRst, 1, c.token);
        return;
    default: return;
    }
}

void conn_rst(Lib& lib, Conn& c, const Header& h)
{
    switch (c.state) {
    case kSynRcvd:
        if (h.token != c.peer_token) return;
        c.state = kClosed;
        st_add32(lib, kStFailed, 1);
        st_add32(lib, kStFailedReset, 1);
        if (c.owner) ctx_closed(*c.owner, kWhyReset);
        return;
    case kSynSent:
        if ((h.opts & rudp::kOptCookie) && h.cookie_len && h.cookie_len <= 32) {
            if (!c.cookie_mem) {
                c.cookie_mem = mem_alloc(lib, h.cookie_len);
                if (!c.cookie_mem) return conn_abort(lib, c, kWhyReset, 0);
            }
            c.cookie.assign(h.cookie.begin(), h.cookie.begin() + h.cookie_len);
            if (c.owner) ctx_post(*c.owner, kQSendSyn);
            return;
        }
        return conn_abort(lib, c, kWhyReset, 0);
    case kEstablished:
    case kCloseWait:
        if (h.token == c.peer_token) conn_abort(lib, c, kWhyReset, 0);
        return;
    default: return;
    }
}

// A packet for this connection (its header first): MSG_MALFORMED for a header that does not parse.
s32 conn_input(Lib& lib, Conn& c, Bytes& pkt)
{
    if (c.state == kIdle) return 0;
    c.rcvd_packets++;
    c.rcvd_bytes += pkt.len;
    Header h;
    const std::optional<u32> used = rudp::decode(pkt.p + pkt.off, pkt.len, h);
    if (!used) return err(CELL_RUDP_ERROR_MSG_MALFORMED);
    pkt.off += *used;
    pkt.len -= *used;
    c.last_rcvd = now_us();
    st_type(lib, h.type, false);
    if (h.flags & rudp::kFlagAck) {
        if (h.type == rudp::kTypeRst) return err(CELL_RUDP_ERROR_MSG_MALFORMED);
        if (conn_ack(lib, c, h) < 0) return 0;
    }
    if (h.type == rudp::kTypeRst) {
        conn_rst(lib, c, h);
        return 0;
    }
    c.window = rudp::decode_window(h.window);
    if (!c.window && c.sq_bytes) {
        if (!c.persist_ms) {
            c.persist_since = now_us();
            c.persist_ms = 3000;
        }
    } else if (c.persist_ms) {
        if (c.sq_bytes) conn_queue_output(c);
        c.persist_ms = 0;
    }
    switch (h.type) {
    case rudp::kTypePrb:
        if (c.state == kEstablished) {
            if (!(h.flags & rudp::kFlagAck) && c.owner) ctx_post(*c.owner, kQProbeReply);
        } else if (c.state == kCloseWait && c.owner) {
            ctx_post(*c.owner, kQSendRst, 1, c.token);
        }
        break;
    case rudp::kTypeUsr: conn_usr(lib, c, h, pkt); break;
    case rudp::kTypeSyn: conn_syn(lib, c, h); break;
    default: break;
    }
    return 0;
}

// The acknowledgement a packet carries: the next expected, the blocks held past a hole, and how long since the data
// it acknowledges came (when it was the newest).
void conn_ack_info(Conn& c, Header& h)
{
    h.ack = c.rcv_nxt;
    h.flags |= rudp::kFlagAck;
    if ((c.flags & kCSack) && c.rq_head && (!c.rq_ordered || c.rq_tail != c.rq_ordered)) {
        Seg* cur = c.rq_ordered ? c.rq_ordered->next : c.rq_head;
        u16 start = cur->seq;
        while (cur && h.nsack <= 7) {
            Seg* nx = cur->next;
            if (!nx) {
                h.sack[h.nsack++] = {start, static_cast<u16>(cur->seq + 1)};
                break;
            }
            if (nx->seq != static_cast<u16>(cur->seq + 1)) {
                h.sack[h.nsack++] = {start, static_cast<u16>(cur->seq + 1)};
                start = nx->seq;
            }
            cur = nx;
        }
    }
    if (h.type == rudp::kTypeUsr && c.rcv_high == c.rcv_last && c.rcv_nxt == static_cast<u16>(c.rcv_last + 1)) {
        h.flags |= rudp::kFlagDelay;
        h.delay = static_cast<u16>((now_us() - c.last_data) / 1000);
    }
}

u32 conn_agg(const Conn& c, const Header& h) { return (h.flags & rudp::kFlagLatency) || (c.flags & kCNodelay) ? 0 : c.aggregation; }

void conn_count_sent(Lib& lib, Conn& c, u8 type, u32 payload)
{
    c.last_sent = now_us();
    c.sent_packets++;
    c.sent_bytes += payload;
    st_type(lib, type, true);
}

void conn_syn_fields(Conn& c, Header& h)
{
    h.type = rudp::kTypeSyn;
    if (c.stream()) {
        h.syn |= rudp::kSynStream | rudp::kSynDelivery | rudp::kSynOrder;
        h.level = 1;
    } else {
        if (c.delivery()) {
            h.syn |= rudp::kSynDelivery;
            h.level = 1;
        } else {
            h.level = 0;
        }
        if (c.order()) h.syn |= rudp::kSynOrder;
    }
    h.mss = static_cast<u16>(c.pool.bufsize + kPayloadSlack);
    h.token = c.token;
    h.opts |= rudp::kOptMss;
}

void conn_send_syn(Lib& lib, Conn& c)
{
    Header h;
    h.seq = c.snd_nxt;
    h.window = rudp::encode_window(c.free_rcv_slots());
    conn_syn_fields(c, h);
    if (!c.cookie.empty()) {
        h.opts |= rudp::kOptCookie;
        h.cookie_len = static_cast<u32>(c.cookie.size());
        std::copy(c.cookie.begin(), c.cookie.end(), h.cookie.begin());
    }
    conn_count_sent(lib, c, h.type, 0);
    if (c.owner) ctx_send(*c.owner, h, nullptr, 0, conn_agg(c, h), nullptr);
}

void conn_send_ack(Lib& lib, Conn& c)
{
    c.delack_since = 0;
    c.flags &= ~kCAckQueued;
    c.delayed_acks = 0;
    Header h;
    if (c.state == kSynSent || c.state == kSynRcvd) conn_syn_fields(c, h);
    h.window = rudp::encode_window(c.free_rcv_slots());
    h.seq = c.snd_nxt;
    conn_ack_info(c, h);
    conn_count_sent(lib, c, h.type, 0);
    if (c.owner) ctx_send(*c.owner, h, nullptr, 0, conn_agg(c, h), nullptr);
}

// A probe (PRB): the window shut, keep-alive; `reply` (an answer to the peer's) carries the acknowledgement.
void conn_send_probe(Lib& lib, Conn& c, bool reply)
{
    Header h;
    h.type = rudp::kTypePrb;
    h.seq = c.snd_nxt;
    h.window = rudp::encode_window(c.free_rcv_slots());
    if (reply) conn_ack_info(c, h);
    conn_count_sent(lib, c, h.type, 0);
    if (c.owner) ctx_send(*c.owner, h, nullptr, 0, conn_agg(c, h), nullptr);
}

void conn_send_rst(Lib& lib, Conn& c, u32 token, u8 code)
{
    c.snd_nxt++;
    Header h;
    h.type = rudp::kTypeRst;
    h.seq = 0;
    h.window = rudp::encode_window(0);
    h.token = token;
    h.code = code;
    conn_count_sent(lib, c, h.type, 0);
    if (c.owner) ctx_send(*c.owner, h, nullptr, 0, 0, nullptr);
}

// What may go now goes: new segments within the peer's window and the congestion window, those to send again.
void conn_output(Lib& lib, Conn& c)
{
    c.flags &= ~kCSendQueued;
    if (!c.sq_bytes) return;
    const u16 base = (c.sq_head->flags & kSegSent) ? c.snd_una : c.snd_nxt;
    const u16 wnd_end = static_cast<u16>(base + c.window);
    const u64 now = now_us();
    const u32 now_ms = static_cast<u32>(now / 1000);
    Seg* s = c.sq_head;
    if (s && c.sq_unsent) {
        u32 bytes = 0;
        for (;;) {
            const u16 f = s->flags;
            const bool sent = f & kSegSent;
            if (sent && !(f & kSegLost)) {
                if (!(f & kSegSacked)) bytes += s->len;
                s = s->next;
                if (!s) break;
                continue;
            }
            u16 seq_for_window = 0;
            if (c.recover < 0) {
                if (!sent) seq_for_window = c.snd_nxt;
                else if ((f & kSegLost) && (f & kSegSacked)) {
                    s->flags &= ~kSegLost;
                    s = s->next;
                    if (!s || !c.sq_unsent) break;
                    continue;
                } else seq_for_window = s->seq;
            } else {
                if (!sent) break;
                if ((f & kSegLost) && (f & kSegSacked)) {
                    s->flags &= ~kSegLost;
                    s = s->next;
                    if (!s || !c.sq_unsent) break;
                    continue;
                }
                seq_for_window = s->seq;
            }
            if (sdiff(seq_for_window, wnd_end) >= 0) break;
            bytes += s->len;
            if (c.reliable() && c.cwnd <= bytes) break;
            Header h;
            h.type = rudp::kTypeUsr;
            Seg* stamp = nullptr;
            if (!(s->flags & kSegSent)) {
                s->seq = c.snd_nxt++;
                if (c.reliable()) {
                    stamp = s;
                    s->stamp = now_ms;
                }
                h.seq = s->seq;
            } else {
                const u8 count = static_cast<u8>(s->retrans >> 24);
                const u32 hi = count == 0xFF ? 0xFF000000u : static_cast<u32>(count + 1) << 24;
                h.seq = s->seq;
                s->retrans = ((now_ms - s->stamp) & 0xFFFFFF) | hi;
                h.retransmission = s->retrans;
                h.ext |= rudp::kExtRetransmission;
            }
            if (s->flags & kSegStamp) {
                h.stamp = now_ms;
                h.ext |= rudp::kExtTimestamp;
            }
            h.window = rudp::encode_window(c.free_rcv_slots());
            if (c.delayed_acks && !(c.flags & kCAckQueued)) {
                conn_ack_info(c, h);
                c.delayed_acks = 0;
                c.delack_since = 0;
            }
            if (s->flags & kSegLatency) h.flags |= rudp::kFlagLatency;
            if (s->flags & kSegMore) h.flags |= rudp::kFlagMore;
            const u32 u = (s->flags & kSegAlign32 ? 1u : 0u) + (s->flags & kSegAlign64 ? 2u : 0u);
            if (u) h.align = static_cast<u8>(1u << (u + 1));
            if (c.persist_ms) {
                c.persist_since = now;
                c.persist_ms = 3000;
            }
            conn_count_sent(lib, c, h.type, s->len);
            if (c.owner) ctx_send(*c.owner, h, s->buf.data() + s->off, s->len, conn_agg(c, h), stamp);
            s->flags = static_cast<u16>((s->flags | kSegSent) & ~kSegLost);
            if (!c.reliable()) {
                Seg* next = s->next;
                c.sq_bytes -= s->len;
                c.snd_una = s->seq;
                c.sq_unsent -= s->len;
                if (c.sq_head == s) {
                    c.sq_head = next;
                    if (!next) {
                        c.sq_tail = nullptr;
                        c.sq_unsent = 0;
                    }
                }
                if ((c.flags & kCFlushing) && (s->flags & kSegFlush)) {
                    c.flags &= ~kCFlushing;
                    if (c.owner) ctx_post(*c.owner, kQFlushed);
                }
                pool_put(c, s);
                if ((c.flags & kCWantWritable) && c.state == kEstablished && c.writable() > 0) {
                    c.flags &= ~kCWantWritable;
                    if (c.owner) ctx_post(*c.owner, kQWritable);
                }
                s = next;
            } else {
                c.sq_unsent -= s->len;
                s = s->next;
            }
            if (!s || !c.sq_unsent) break;
        }
    }
    if (!c.rexmit_at) {
        c.backoff = 0;
        c.rexmit_at = c.sq_bytes ? now_us() + rto_capped(c) : 0;
    }
}

// A write into the send queue: a stream's bytes fill the last unsent segment first; each segment a payload.
s32 conn_enqueue(Lib& lib, Conn& c, Bytes& in, u8 wflags)
{
    u32 free = 0;
    if (c.state == kEstablished) {
        const u32 n = c.slots(c.sndbuf);
        free = n > c.pool.nsend ? n - c.pool.nsend : 0;
    }
    if (c.stream() && c.sq_tail && !(c.sq_tail->flags & kSegSent) && c.sq_tail->len + kPayloadSlack < c.segsize) {
        const u32 take = std::min(in.len, c.segsize - c.sq_tail->len - kPayloadSlack);
        seg_append(*c.sq_tail, in.p + in.off, take);
        in.off += take;
        in.len -= take;
        c.sq_bytes += take;
        c.sq_unsent += take;
        if (wflags & kMsgWithTimestamp) c.sq_tail->flags |= kSegStamp;
    }
    if (!in.len || !free) return 0;
    for (;;) {
        Seg* s = pool_get(lib, c, true);
        if (!s) return err(CELL_RUDP_ERROR_MEMORY);
        if (c.stream()) s->flags |= kSegMore;
        const u32 take = std::min(in.len, c.segsize - kPayloadSlack);
        s->next = nullptr;
        seg_append(*s, in.p + in.off, take);
        in.off += take;
        in.len -= take;
        if (wflags & kMsgLatencyCritical) s->flags |= kSegLatency;
        if (wflags & kMsgAlign32) s->flags |= kSegAlign32;
        if (wflags & kMsgAlign64) s->flags |= kSegAlign64;
        if (wflags & kMsgWithTimestamp) s->flags |= kSegStamp;
        if (!(s->flags & kSegLatency) || c.stream() || c.order()) {
            if (c.sq_tail) c.sq_tail->next = s;
            else c.sq_head = s;
            c.sq_tail = s;
        } else {
            Seg *prev = nullptr, *cur = c.sq_head;
            bool placed = false;
            while (cur) {
                if (!(cur->flags & kSegSent) && !(cur->flags & kSegLatency)) {
                    if (!prev) {
                        s->next = c.sq_head;
                        c.sq_head = s;
                    } else {
                        prev->next = s;
                        s->next = cur;
                    }
                    placed = true;
                    break;
                }
                prev = cur;
                cur = cur->next;
            }
            if (!placed) {
                if (prev) prev->next = s;
                else c.sq_head = s;
                c.sq_tail = s;
            }
        }
        c.sq_bytes += take;
        c.sq_unsent += take;
        if (!in.len || !--free) break;
        if (!c.stream()) s->flags |= kSegMore;
    }
    return 0;
}

s32 conn_write(Lib& lib, Conn& c, Bytes& in, u8 wflags)
{
    if (c.state != kEstablished) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    const u32 len = in.len;
    if (!len) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    const u32 n = c.slots(c.sndbuf);
    if (n <= c.pool.nsend || !(n - c.pool.nsend)) return err(CELL_RUDP_ERROR_WOULDBLOCK);
    const bool whole = c.stream() || (c.delivery() && c.order());
    const u32 room = whole ? c.payload() * (n - c.pool.nsend) : c.payload();
    if (!room) return err(CELL_RUDP_ERROR_WOULDBLOCK);
    if (!c.stream()) {
        const u32 most = c.delivery() && c.order() ? c.payload() * c.peer_window : c.payload();
        if (most < len) return err(CELL_RUDP_ERROR_MSG_TOO_LARGE);
        if (room < len) {
            c.flags |= kCWantWritable;
            return err(CELL_RUDP_ERROR_WOULDBLOCK);
        }
    }
    if (const s32 r = conn_enqueue(lib, c, in, wflags); r < 0) return r;
    if (!c.window && c.sq_unsent && !c.persist_ms) {
        c.persist_since = now_us();
        c.persist_ms = 3000;
    }
    conn_queue_output(c);
    if (c.state == kEstablished || c.state == kCloseWait) {
        if (c.state != kEstablished || c.writable() <= 0) c.flags |= kCWantWritable;
    }
    const u32 written = len - in.len;
    st_add32(lib, kStSentUserPackets, 1);
    st_add64(lib, kStSentUserBytes, written);
    if ((wflags & kMsgLatencyCritical) || (c.flags & kCNodelay)) st_add32(lib, kStSentLatencyCritical, 1);
    const u32 lv = kStLevels + 0x20 * level_of(c);
    st_add32(lib, lv, 1);
    st_add64(lib, lv + 8, written);
    return static_cast<s32>(written);
}

// Reads the next message (or a stream's bytes) into `out`; `info` gets the read information.
s32 conn_read(Lib& lib, Conn& c, Bytes& out, u8* info)
{
    if (info) std::memset(info, 0, 12);
    if (c.state != kEstablished && c.state != kCloseWait) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    const s32 avail = static_cast<s32>(c.stream() ? c.rq_ordered_bytes : c.readable);
    if (!avail) return c.state == kEstablished ? err(CELL_RUDP_ERROR_WOULDBLOCK) : err(CELL_RUDP_ERROR_END_OF_DATA);
    if (out.cap - out.off == out.len) return err(CELL_RUDP_ERROR_BUFFER_TOO_SMALL);
    out.off = out.len = 0;
    bool latency = false;
    u32 n;
    if (!c.stream()) {
        n = c.readable;
        if (out.room() < n) return err(CELL_RUDP_ERROR_BUFFER_TOO_SMALL);
    } else {
        n = std::min(out.room(), c.rq_ordered_bytes);
    }
    while (n) {
        Seg* s = c.rq_head;
        if (!s) break;
        if (c.rq_next_latency) {
            if (c.stream() || c.order()) {
                c.rq_next_latency = nullptr;
                break;
            }
            s = c.rq_next_latency;
        }
        const u32 take = std::min(n, s->len);
        if (sdiff(c.rcv_low, s->seq) < 1) c.rcv_low = static_cast<u16>(s->seq + 1);
        n -= take;
        seg_consume(*s, out.p + out.off + out.len, take);
        out.len += take;
        c.rq_bytes -= take;
        if (c.stream() || c.delivery()) c.rq_ordered_bytes -= take;
        if (c.rq_next_latency) {
            s->flags = static_cast<u16>((s->flags & ~kSegSacked) | kSegSent);
            c.rq_next_latency = nullptr;
            if (c.rq_latency) c.rq_latency--;
        }
        if (s->flags & kSegLatency) latency = true;
        if (info) {
            info[1] = static_cast<u8>(s->retrans >> 24);
            ps_st16(info + 2, static_cast<u16>(s->retrans));
            info[4] = static_cast<u8>(s->retrans >> 16);
            if (s->flags & kSegLatency) info[5] |= 0x08;
            ps_st16(info + 6, s->seq);
            if (s->flags & kSegStamp) {
                info[5] |= 0x40;
                ps_st32(info + 8, s->stamp);
            }
        }
        Seg* head = c.rq_head;
        if (head->len) continue;
        c.rq_head = head->next;
        if (!c.rq_head) {
            c.rq_bytes = 0;
            c.rq_tail = nullptr;
            if (c.stream() || c.delivery()) {
                c.rq_ordered = nullptr;
                c.rq_ordered_bytes = 0;
            }
        } else if ((c.stream() || c.delivery()) && c.rq_ordered == head) {
            c.rq_ordered = nullptr;
            c.rq_ordered_bytes = 0;
        }
        pool_put(c, head);
    }
    st_add32(lib, kStRcvdUserPackets, 1);
    st_add64(lib, kStRcvdUserBytes, out.len);
    if (latency) st_add32(lib, kStRcvdLatencyCritical, 1);
    const u32 lv = kStLevels + 0x20 * level_of(c) + 0x10;
    st_add32(lib, lv, 1);
    st_add64(lib, lv + 8, out.len);
    c.readable = 0;
    c.flags &= ~0x2u;
    conn_update_readable(c);
    return static_cast<s32>(out.len);
}

s32 conn_flush(Conn& c)
{
    if (c.state != kEstablished) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    if (c.flags & kCFlushing) return err(CELL_RUDP_ERROR_IN_PROGRESS);
    if (!c.sq_tail) {
        if (c.owner) ctx_post(*c.owner, kQFlushed);
    } else {
        c.flags |= kCFlushing;
        c.sq_tail->flags |= kSegFlush;
    }
    return 0;
}

void conn_keepalive(Lib& lib, Conn& c)
{
    const u64 now = now_us();
    if (!c.ka_interval) return;
    u64 next;
    if (!c.ka_first) {
        const u64 iv = u64{c.ka_interval} * 1000;
        if (iv + c.last_rcvd < now) goto probe;
        next = iv + c.last_sent;
    } else {
        if (c.ka_first < c.ka_heard) {
            c.ka_next = c.ka_first = c.ka_last = c.ka_heard = 0;
            c.ka_count = 0;
            return;
        }
        if (c.ka_first + u64{c.ka_timeout} * 1000 < now) return conn_abort(lib, c, kWhyKeepAlive, 1);
        next = c.ka_last + c.ka_next;
    }
    if (now <= next) return;
probe:
    const u64 t = now_us();
    conn_send_probe(lib, c, false);
    if (!c.ka_first) c.ka_first = t;
    c.ka_last = t;
    c.ka_next = rto_capped(c, c.ka_count);
    c.ka_count++;
}

// The retransmission timer came due.
void conn_timeout(Lib& lib, Conn& c)
{
    const auto rearm = [&](u32 shift) { c.rexmit_at = now_us() + rto_capped(c, shift); };
    switch (c.state) {
    case kClosed:
        if (!(c.flags & kCActivated)) return;
        if (c.deadline && c.deadline <= now_us()) return conn_abort(lib, c, kWhyTimeout, 0);
        c.backoff++;
        return rearm(c.backoff);
    case kSynSent:
        if (c.deadline && c.deadline <= now_us()) return conn_abort(lib, c, kWhyTimeout, 2);
        c.backoff++;
        conn_send_syn(lib, c);
        return rearm(c.backoff);
    case kSynRcvd:
        if (c.deadline && c.deadline <= now_us()) return conn_abort(lib, c, kWhyTimeout, 2);
        c.backoff++;
        conn_send_ack(lib, c);
        return rearm(c.backoff);
    case kEstablished: {
        const u32 half = (c.sq_bytes - c.sq_unsent) >> 1, two = c.segsize << 1;
        c.ssthresh = std::max(half, two);
        c.cwnd = c.segsize;
        if (c.reliable()) {
            for (Seg* s = c.sq_head; s && (s->flags & kSegSent); s = s->next) {
                if (s->flags & (kSegSacked | kSegLost)) continue;
                c.retransmissions++;
                s->flags |= kSegLost;
                st_add32(lib, kStRetransmitted, 1);
                c.sq_unsent += s->len;
            }
        }
        c.backoff++;
        c.dupacks = 0;
        c.recover = -1;
        rearm(c.backoff);
        if (c.reliable() && !(c.flags & kCSendQueued)) conn_output(lib, c);
        return;
    }
    case kCloseWait:
        if (c.deadline >= now_us()) return rearm(0);
        c.backoff = 0;
        c.state = kClosed;
        c.rexmit_at = 0;
        if (c.owner) ctx_post(*c.owner, kQCleanup);
        return;
    default: return;
    }
}

// The context's timer: every half round trip (16 ms at least; 125 ms before the first sample).
void conn_tick(Lib& lib, Conn& c, u64& interval)
{
    const u64 now = now_us();
    if (c.rexmit_at && c.rexmit_at < now) {
        c.rexmit_at = 0;
        conn_timeout(lib, c);
    }
    u64 iv = 125000;
    if (c.srtt) iv = std::max<u64>(c.srtt >> 1, 16000);
    c.min_var = iv;
    interval = iv;
    if (c.state != kEstablished) return;
    if (!(c.flags & kCSendQueued)) conn_output(lib, c);
    if (c.delayed_acks && !(c.flags & kCAckQueued) && c.delack_since + 100000 < now) conn_send_ack(lib, c);
    if (c.persist_ms && u64{c.persist_ms} * 1000 + c.persist_since < now) {
        conn_send_probe(lib, c, false);
        c.persist_since = now;
        c.persist_ms = std::min<u32>(c.persist_ms << 1, 16000);
    }
    conn_keepalive(lib, c);
}

// ------------------------------------------------------------------------------------------------- guest calls

// A guest function the game gave (a handler, its allocator): called when its descriptor and entry are guest memory,
// else logged and skipped (D-042): nullopt.
std::optional<u64> call_game(u32 opd, std::initializer_list<u64> args, std::string_view what)
{
    if (!t_ppu) PS_FATAL(kChan, "{}: no PPU thread to call it on", what);
    lv2::PpuThread& t = thread_of(*t_ppu);
    if (!opd || !guest_readable(t, opd, 8) || !guest_readable(t, ps_read32(t.kernel.base(), opd), 4)) {
        PS_LOG_ERROR(kChan, "{} {:#x} is not a function in guest memory: not called", what, opd);
        return std::nullopt;
    }
    return call_guest(*t_ppu, opd, args);
}

// Guest memory for a datagram on the calling thread: its stack for a small one, else a block of the main area.
class Scratch {
public:
    Scratch(PPUContext& ctx, u32 size)
    {
        if (size <= 4096) {
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

// librudp's sockets go through libnet, as its imports do (sys_net's exports: socket, bind, getsockname, setsockopt,
// getsockopt, socketselect, recvfrom, sendto, socketclose): without the game's sys_net_initialize_network_ex they fail
// as Sony's do (GetLocalInfo's address stays zero, the internal I/O thread gets no wake-up socket). The answer: >= 0,
// or negative with libnet's errno set (net_errno).
s32 libnet(std::string_view name, std::initializer_list<u64> args)
{
    PPUContext& ctx = *t_ppu;
    const Function* fn = registry().find("sys_net", recomp::nid_of(name));
    if (!fn) PS_FATAL(kChan, "sys_net::{}, which librudp imports, is not pspocket's", name);
    u64 saved[8];
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 3;
    for (const u64 a : args) ctx.gpr[i++] = a;
    fn->fn(ctx, guest_base(ctx));
    const s32 r = static_cast<s32>(ctx.gpr[3]);
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}
s32 net_errno()
{
    const u32 loc = static_cast<u32>(libnet("_sys_net_errno_loc", {}));
    return loc ? static_cast<s32>(ps_read32(guest_base(*t_ppu), loc)) : 0;
}
constexpr s32 kNetEbadf = 9, kNetEwouldblock = 35;

// ------------------------------------------------------------------------------------------------- contexts

void ctx_post(Ctx& x, u8 type, u8 sub, u32 arg)
{
    if (!x.net) return;
    sched_post(lib_of(), *x.net, {x.target, type, sub, arg});
}

void ctx_established(Ctx& x)
{
    if (x.parent) sched_post(lib_of(), *x.net, {x.parent->target, kQChildEstablished, 0, static_cast<u32>(x.parent->id)});
    else ctx_post(x, kQEstablished);
}

void ctx_closed(Ctx& x, u8 why)
{
    if (x.parent) sched_post(lib_of(), *x.net, {x.parent->target, kQChildClosed, 0, static_cast<u32>(x.id)});
    else ctx_post(x, kQClosed, why);
}

void agg_send(Lib& lib, Net& net, const AggKey& key, s32 ctx_id, u8 mux, u16 src, u16 dst, const Header& h,
    const u8* payload, u32 len, u32 agg_ms, Seg* stamp);
void agg_forget_stamps(Net& net, const AggKey& key, u16 vport);

void ctx_send(Ctx& x, const Header& h, const u8* payload, u32 len, u32 agg_ms, Seg* stamp)
{
    if (!x.net) return;
    AggKey key;
    key.soc = x.soc;
    key.addr = x.peer.raw;
    agg_send(lib_of(), *x.net, key, x.id, x.mux, x.vport, x.peer.vport, h, payload, len, agg_ms, stamp);
}

void ctx_drop_queue(Ctx& x)
{
    if (!x.net) return;
    AggKey key;
    key.soc = x.soc;
    key.addr = x.peer.raw;
    agg_forget_stamps(*x.net, key, x.vport);
}

// The token and first sequence number of a handshake: from SHA-256 over the socket, the local address, the vport,
// the time and a random number (librudp's way; the values only need to be hard to guess).
void ctx_init_params(Ctx& x, u16 (&p)[3])
{
    Lib& lib = lib_of();
    std::array<u8, 4 + 0x22 + 2 + 8 + 4> in{};
    ps_st32(in.data(), static_cast<u32>(x.soc));
    std::memcpy(in.data() + 4, x.local.data(), 0x22);
    ps_st16(in.data() + 0x26, x.vport);
    ps_st64(in.data() + 0x28, now_us());
    ps_st32(in.data() + 0x30, static_cast<u32>(lib.rng()));
    const recomp::Sha256Digest d = recomp::Sha256::hash(std::span<const u8>(in.data(), in.size()));
    p[0] = ps_ld16(d.data());
    p[1] = ps_ld16(d.data() + 2);
    p[2] = ps_ld16(d.data() + 4);
}

// A listener's cookie: the millisecond clock, a random word and 24 bytes of SHA-256 over them and its secret.
std::array<u8, 32> make_cookie(Ctx& x)
{
    Lib& lib = lib_of();
    std::array<u8, 12> in{};
    ps_st32(in.data(), now_ms32());
    ps_st32(in.data() + 4, static_cast<u32>(lib.rng()));
    ps_st32(in.data() + 8, x.secret);
    const recomp::Sha256Digest d = recomp::Sha256::hash(std::span<const u8>(in.data(), in.size()));
    std::array<u8, 32> out{};
    std::memcpy(out.data(), d.data(), 32);
    std::memcpy(out.data(), in.data(), 8);
    return out;
}

// 0: valid, 1: forged, 2: expired (older than the cookie's lifetime, option 13).
u32 check_cookie(Ctx& x, const u8* cookie)
{
    const u32 stamp = ps_ld32(cookie);
    if (static_cast<s32>(now_ms32() - (x.cookie_life * 1000 + stamp)) >= 0) return 2;
    std::array<u8, 12> in{};
    std::memcpy(in.data(), cookie, 8);
    ps_st32(in.data() + 8, x.secret);
    const recomp::Sha256Digest d = recomp::Sha256::hash(std::span<const u8>(in.data(), in.size()));
    return std::memcmp(cookie + 8, d.data() + 8, 24) ? 1 : 0;
}

// The events a context's poll set would report now.
u16 ctx_ready(const Ctx& x)
{
    if (x.flags & kXListening) {
        for (const Ctx::Pending& p : x.pending)
            if (p.ctx->c.state == kEstablished) return kPollRead;
        return 0;
    }
    if (x.c.state == kEstablished) {
        u16 r = x.c.readable_size() > 0 ? kPollRead : 0;
        if (x.c.writable() >= 1) r |= kPollWrite;
        return r;
    }
    if (x.c.state == kCloseWait) {
        const s32 n = x.c.readable_size();
        return n > 0 || n == err(CELL_RUDP_ERROR_END_OF_DATA) ? kPollRead : 0;
    }
    return 0;
}

u32 api_state(const Ctx& x) { return (x.flags & kXListening) ? kListening : x.c.state <= 5 ? x.c.state : 0; }

PollSet* poll_find(Net& net, s32 id)
{
    const auto it = net.polls.find(id);
    return it == net.polls.end() ? nullptr : it->second.first.get();
}

void poll_signal(PollSet& p, s32 ctx, u16 events);

void ctx_poll_notify(Ctx& x, u16 events)
{
    if (!x.net) return;
    if (PollSet* p = poll_find(*x.net, x.poll)) poll_signal(*p, x.id, events);
    else x.poll = -1;
}

void ctx_call_handler(Ctx& x, u32 event, u32 code)
{
    if (!x.handler || (x.flags & kXTerminated)) return;
    call_game(x.handler, {sext(x.id), event, sext(static_cast<s32>(code)), x.arg}, "the context handler");
}

void ctx_wake(Ctx& x, u8 bits)
{
    x.wake |= bits;
    x.cond.signal_all();
}

s32 ctx_delete(Lib& lib, Net& net, s32 id);
void ctx_detach_child(Lib& lib, Ctx& parent, const Addr& peer);
void net_unregister_peer(Lib& lib, Net& net, Ctx& x);
void conn_cookies(Lib& lib, Ctx& x);

// A context's queued event (librudp's: what a blocked call, a poll set or the handler hears, and the work the
// connection queued).
void ctx_on_event(Lib& lib, Ctx& x, const Event& e)
{
    // An event the game hears: a blocked call wakes (and the poll set hears it), else the poll set or the handler.
    const auto tell = [&](u8 wake, u16 poll_extra, u32 event, u32 code, bool check_terminated_for_poll) {
        if (x.blocking) {
            ctx_wake(x, wake);
            if (x.poll >= 0 && !(check_terminated_for_poll && (x.flags & kXTerminated)))
                ctx_poll_notify(x, ctx_ready(x) | poll_extra);
            return;
        }
        if (x.poll >= 0) {
            if (!(check_terminated_for_poll && (x.flags & kXTerminated))) ctx_poll_notify(x, ctx_ready(x) | poll_extra);
            return;
        }
        ctx_call_handler(x, event, code);
    };
    switch (e.type) {
    case kQEstablished: return tell(kWakeEstablished, kPollWrite, kEvEstablished, 0, false);
    case kQClosed:
        if (x.flags & kXPeer) {
            x.flags &= ~kXPeer;
            net_unregister_peer(lib, *x.net, x);
        }
        x.error = static_cast<s32>(error_of(e.sub > 8 ? kWhyInternal : e.sub));
        return tell(kWakeClosed, kPollWrite, kEvClosed, static_cast<u32>(x.error), true);
    case kQError:
        x.error = static_cast<s32>(error_of(e.sub > 8 ? kWhyInternal : e.sub));
        return tell(kWakeClosed, kPollRead, kEvClosed, static_cast<u32>(x.error), true);
    case kQReadable:
        if (!x.c.readable_size()) return;
        return tell(kWakeReadable, kPollRead, kEvReadable, 0, false);
    case kQWritable:
        if (!x.c.writable()) return;
        return tell(kWakeWritable, kPollWrite, kEvWritable, 0, false);
    case kQFlushed: return tell(kWakeFlushed, kPollFlush, kEvFlushed, 0, false);
    case kQSendSyn: return conn_send_syn(lib, x.c);
    case kQSendAck: return conn_send_ack(lib, x.c);
    case kQSendRst: return conn_send_rst(lib, x.c, e.arg, e.sub);
    case kQOutput: return conn_output(lib, x.c);
    case kQProbeReply: return conn_send_probe(lib, x.c, true);
    case kQChildEstablished: return tell(kWakeAcceptable, 0, kEvAcceptable, 0, false);
    case kQChildClosed: {
        const s32 child = static_cast<s32>(e.arg);
        const auto it = x.net->ctxs.find(child);
        if (it != x.net->ctxs.end()) {
            Ctx* c = it->second.first;
            for (auto p = x.pending.begin(); p != x.pending.end(); ++p) {
                if (addr_equal(p->ctx->peer.raw, c->peer.raw) && p->ctx->peer.vport == c->peer.vport &&
                    p->ctx->id == child) {
                    mem_free(lib, p->mem);
                    x.pending.erase(p);
                    break;
                }
            }
        }
        if (child != -1) ctx_delete(lib, *x.net, child);
        return;
    }
    case kQCleanup:
        if (x.flags & kXPeer) {
            x.flags &= ~kXPeer;
            net_unregister_peer(lib, *x.net, x);
        }
        if (x.flags & kXTerminated) {
            if (x.id != -1) ctx_delete(lib, *x.net, x.id);
            return;
        }
        if (x.parent) ctx_detach_child(lib, *x.parent, x.peer);
        x.parent = nullptr;
        return;
    case kQCookies: return conn_cookies(lib, x);
    case kQMemory:
    case kQBadSocket: {
        x.error = static_cast<s32>(e.type == kQMemory ? CELL_RUDP_ERROR_MEMORY : CELL_RUDP_ERROR_INVALID_SOCKET);
        if (x.blocking) {
            ctx_wake(x, kWakeCancel);
            if (x.poll >= 0) ctx_poll_notify(x, ctx_ready(x) | kPollError);
            return;
        }
        if (x.poll >= 0) return ctx_poll_notify(x, ctx_ready(x) | kPollError);
        return ctx_call_handler(x, kEvError, static_cast<u32>(x.error));
    }
    default: return;
    }
}

void ctx_on_timer(Lib& lib, Ctx& x, u64& interval)
{
    if (x.valid) conn_tick(lib, x.c, interval);
}

// A listener lets a child go (it ended, or went away): from those waiting and those accepted. A terminated listener
// with none left goes too.
void ctx_detach_child(Lib& lib, Ctx& parent, const Addr& peer)
{
    if (!parent.valid) return;
    for (auto p = parent.pending.begin(); p != parent.pending.end(); ++p) {
        if (addr_equal(p->ctx->peer.raw, peer.raw) && p->ctx->peer.vport == peer.vport) {
            mem_free(lib, p->mem);
            parent.pending.erase(p);
            break;
        }
    }
    const auto it = parent.accepted.find(peer);
    if (it != parent.accepted.end()) {
        mem_free(lib, it->second.mem);
        parent.accepted.erase(it);
    }
    if ((parent.flags & kXTerminated) && parent.pending.empty() && parent.accepted.empty())
        ctx_post(parent, kQCleanup);
}

// A listener's cookies go out: a RST (reason 2) with 32 bytes, one per SYN that came without a valid one.
void net_send_control(Lib& lib, Net& net, u8 mux, const Header& h, s32 soc, u16 src, const Addr& to);
void conn_cookies(Lib& lib, Ctx& x)
{
    if (!x.cookies) return;
    Ctx::CookieRing& r = *x.cookies;
    while (r.head != r.tail || r.full) {
        r.full = false;
        const Addr to = r.entries[r.head];
        if (++r.head == r.entries.size()) r.head = 0;
        const std::array<u8, 32> cookie = make_cookie(x);
        Header h;
        h.type = rudp::kTypeRst;
        h.seq = 0;
        h.window = rudp::encode_window(0);
        h.opts |= rudp::kOptCookie;
        h.cookie_len = 32;
        h.cookie = cookie;
        h.token = 0;
        h.code = kWhyRefused;
        if (x.net) net_send_control(lib, *x.net, x.mux, h, x.soc, x.vport, to);
        if (!x.cookies) return;
    }
    if (!x.cookie_life) {
        mem_free(lib, r.mem_buf);
        mem_free(lib, r.mem_obj);
        x.cookies.reset();
    }
}

bool ctx_cookie_ring(Lib& lib, Ctx& x)
{
    if (x.cookies) return true;
    Block obj = mem_alloc(lib, 0x14);
    if (!obj) return false;
    Block buf = mem_alloc(lib, 0x900);
    if (!buf) {
        mem_free(lib, obj);
        return false;
    }
    x.cookies = std::make_unique<Ctx::CookieRing>();
    x.cookies->mem_obj = obj;
    x.cookies->mem_buf = buf;
    return true;
}

// The context's peer (Initiate's, Activate's, a child's): a P2P context's peer shares its vport. The aggregator
// learns the destination.
s32 net_register_peer(Lib& lib, Net& net, Ctx& x);
s32 ctx_set_peer(Lib& lib, Ctx& x, const std::array<u8, 0x22>& addr, u16 vport)
{
    if (x.mux == 2) {
        if (x.flags & kXListening) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    } else {
        vport = x.vport;
    }
    if (x.flags & kXPeer) {
        return addr_equal(addr, x.peer.raw) && x.peer.vport == vport ? 0 : err(CELL_RUDP_ERROR_IN_PROGRESS);
    }
    x.flags |= kXPeer;
    x.peer.raw = {};
    std::memcpy(x.peer.raw.data(), addr.data(), std::min<usize>(addr[0], 0x22));
    x.peer.vport = vport;
    if (!x.net) return 0;
    return net_register_peer(lib, *x.net, x);
}

// Makes a context (its ID the next of a count that never goes back, from 0): MEMORY when the allocator gives none.
s32 ctx_create(Lib& lib, Net& net, Ctx*& out)
{
    out = nullptr;
    const s32 id = net.next_ctx;
    net.next_ctx = (net.next_ctx + 1) & 0x7FFFFFFF;
    Block mem = mem_alloc(lib, 0x280);
    if (!mem) return err(CELL_RUDP_ERROR_MEMORY);
    auto* x = new Ctx;
    x->mem = mem;
    x->net = &net;
    x->id = id;
    x->c.owner = x;
    s32 r = 0;
    Target t;
    t.kind = Target::Kind::Ctx;
    t.ctx = x;
    if (sched_register(lib, net.sched, t, x->target) < 0) {
        x->target = 0;
        r = err(CELL_RUDP_ERROR_INTERNAL);
    } else if (const s32 a = sched_arm(lib, net.sched, x->target, 16000); a < 0) {
        if (a == err(CELL_RUDP_ERROR_MEMORY)) {
            ctx_post(*x, kQMemory);
            x->error = err(CELL_RUDP_ERROR_MEMORY);
        } else {
            x->error = err(CELL_RUDP_ERROR_INTERNAL);
        }
    }
    if (!r) r = x->error;
    if (!r) {
        Block node = mem_alloc(lib, 0x20);
        if (!node) r = err(CELL_RUDP_ERROR_MEMORY);
        else net.ctxs.emplace(id, std::make_pair(x, node));
    }
    if (r) {
        // librudp deletes the context it could not finish
        x->c.owner = nullptr;
        if (x->target) {
            sched_disarm(net.sched, x->target);
            sched_unregister(lib, net.sched, x->target);
        }
        mem_free(lib, x->mem);
        delete x;
        return r;
    }
    out = x;
    return 0;
}

// A context goes: its waiters return, it leaves its listener, the scheduler, the aggregator; its queues and records go.
void ctx_destroy(Lib& lib, Ctx* x)
{
    lv2::PpuThread& t = thread_of(*t_ppu);
    while (x->waiters > 0) {
        x->wake |= kWakeCancel;
        x->cond.signal_all();
        x->cond.wait(lib.lock, t, 0);
    }
    x->wake &= ~kWakeCancel;
    if (x->valid && x->parent) {
        ctx_detach_child(lib, *x->parent, x->peer);
        x->parent = nullptr;
    }
    Net* net = x->net;
    if (x->target && net) {
        sched_disarm(net->sched, x->target);
        sched_unregister(lib, net->sched, x->target);
    }
    x->c.owner = nullptr;
    if (x->flags & kXPeer) {
        x->flags &= ~kXPeer;
        if (net) net_unregister_peer(lib, *net, *x);
    }
    if (x->cookies) {
        mem_free(lib, x->cookies->mem_buf);
        mem_free(lib, x->cookies->mem_obj);
        x->cookies.reset();
    }
    x->valid = false;
    for (auto& [k, n] : x->accepted) {
        if (n.ctx->parent == x) n.ctx->parent = nullptr;
        mem_free(lib, n.mem);
    }
    x->accepted.clear();
    for (Ctx::Pending& p : x->pending) {
        if (p.ctx->parent == x) p.ctx->parent = nullptr;
        mem_free(lib, p.mem);
    }
    x->pending.clear();
    if (net)
        for (auto& [id, e] : net->ctxs)
            if (e.first->parent == x) e.first->parent = nullptr;
    conn_reset(lib, x->c);
    conn_free_cookie(lib, x->c);
    pool_clear(lib, x->c);
    mem_free(lib, x->mem);
    delete x;
}

void net_ctx_removed(Lib& lib, Net& net, Ctx& x);

s32 ctx_delete(Lib& lib, Net& net, s32 id)
{
    const auto it = net.ctxs.find(id);
    if (it == net.ctxs.end()) return err(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    Ctx* x = it->second.first;
    net_ctx_removed(lib, net, *x);
    ctx_destroy(lib, x);
    const auto again = net.ctxs.find(id);
    if (again != net.ctxs.end()) {
        mem_free(lib, again->second.second);
        net.ctxs.erase(again);
    }
    return 0;
}

// The context with this ID (one terminated and lingering only when `any`): INVALID_CONTEXT_ID otherwise.
Ctx* ctx_find(Net& net, s32 id, bool any, s32* error)
{
    const auto it = net.ctxs.find(id);
    Ctx* x = it == net.ctxs.end() ? nullptr : it->second.first;
    if (x && !any && x->valid && (x->flags & kXTerminated)) x = nullptr;
    if (error) *error = x ? 0 : err(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    return x;
}

// Terminate: a connection resets its peer and lingers until its close-wait time is over; a listener ends its children.
s32 ctx_terminate(Lib& lib, Ctx& x)
{
    if (x.flags & kXTerminated) return err(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    x.flags |= kXTerminated;
    s32 r = 0;
    if (!(x.flags & kXListening)) {
        r = conn_terminate(lib, x.c);
    } else {
        std::vector<Ctx*> accepted;
        for (auto& [k, n] : x.accepted) accepted.push_back(n.ctx);
        for (Ctx* c : accepted) ctx_terminate(lib, *c);
        std::vector<Ctx*> pending;
        for (Ctx::Pending& p : x.pending) pending.push_back(p.ctx);
        for (Ctx* c : pending) {
            ctx_terminate(lib, *c);
            ctx_post(*c, kQCleanup);
        }
        if (x.accepted.empty() && x.pending.empty()) ctx_post(x, kQCleanup);
    }
    if (x.blocking) ctx_wake(x, kWakeCancel);
    return r;
}

// ------------------------------------------------------------------------------------------------- polls

void poll_signal(PollSet& p, s32 ctx, u16 events)
{
    const auto it = p.entries.find(ctx);
    if (it == p.entries.end()) return;
    PollSet::Entry& e = it->second;
    const u16 v = static_cast<u16>((events & e.req) | e.ret);
    e.ret = v;
    if (!(v & 0x8000) && (v & 0x7FFF) && p.push(ctx)) {
        e.ret = static_cast<u16>(v | 0x8000);
        p.cond.signal();
    }
}

// Ends the waits in a poll set: PollCancel's (a cancel the next wait sees when nobody waits), or the set going.
void poll_cancel(Lib& lib, PollSet& p, bool cancel, bool dying)
{
    lv2::PpuThread& t = thread_of(*t_ppu);
    if (!p.waiters) {
        if (cancel) p.cancel = true;
    } else if (p.waiters > 0) {
        if (!dying) {
            do {
                if (p.dying) {
                    p.cond.signal_all();
                    return;
                }
                p.cancelling++;
                p.cond.signal_all();
                p.cond.wait(lib.lock, t, 0);
                p.cancelling--;
            } while (p.waiters > 0);
        } else {
            do {
                p.cancelling++;
                p.cond.signal_all();
                p.cond.wait(lib.lock, t, 0);
                p.cancelling--;
            } while (p.waiters > 0);
        }
    }
    if (p.dying) {
        if (!dying) {
            p.cond.signal_all();
        } else {
            while (p.cancelling > 0) {
                p.cond.signal_all();
                p.cond.wait(lib.lock, t, 0);
            }
        }
    }
}

void poll_destroy(Lib& lib, Net& net, PollSet& p)
{
    p.dying = true;
    poll_cancel(lib, p, false, true);
    for (auto& [ctx, e] : p.entries) {
        if (Ctx* x = ctx_find(net, ctx, true, nullptr)) x->poll = -1;
    }
    mem_free(lib, p.mem_ring);
    p.ring.clear();
    for (auto& [ctx, e] : p.entries) mem_free(lib, e.mem);
    p.entries.clear();
}

// ------------------------------------------------------------------------------------------------- datagrams out

void net_socket_error(Lib& lib, Net& net, s32 soc);

// One datagram to lv2 or to the game: without the internal I/O thread the event handler sends it
// (CELL_RUDP_EVENT_SEND); with it, sendto (the handler first hears event 100 and may refuse).
s32 net_send_one(Lib& lib, Net& net, const rudp::Packet& p)
{
    const u32 len = static_cast<u32>(p.bytes.size());
    Scratch s(*t_ppu, 16 + len);
    if (!s.addr) {
        PS_LOG_ERROR(kChan, "no guest memory to send a datagram of {} bytes from", len);
        return 0;
    }
    u8* base = guest_base(*t_ppu);
    std::memcpy(base + s.addr, p.to.data(), 16);
    std::memcpy(base + s.addr + 16, p.bytes.data(), len);
    const u32 addrlen = p.to[0];
    s32 result = 0;
    if (!net.io_enabled) {
        if (net.handler) {
            const std::optional<u64> r = call_game(net.handler,
                {kEvSend, sext(p.soc), s.addr + 16u, len, s.addr, addrlen, net.handler_arg}, "the event handler");
            const s32 v = r ? static_cast<s32>(*r) : 0;
            if (v >= 0) result = v;
            else if (v == err(CELL_RUDP_ERROR_INVALID_SOCKET)) {
                net_socket_error(lib, net, p.soc);
                result = v;
            }
        }
    } else {
        if (net.handler) {
            const std::optional<u64> r = call_game(net.handler,
                {kEvBeforeSend, sext(p.soc), s.addr + 16u, len, s.addr, addrlen, net.handler_arg},
                "the event handler");
            if (r && static_cast<s32>(*r) < 0) return static_cast<s32>(len);
        }
        const s32 r = libnet("sendto", {sext(p.soc), s.addr + 16u, len, 0, s.addr, 16});
        if (r < 0 && net_errno() != kNetEwouldblock) {
            net_socket_error(lib, net, p.soc);
            result = -1;
        }
    }
    st_add32(lib, kStSentUdpPackets, 1);
    st_add64(lib, kStSentUdpBytes, len);
    return result;
}

s32 net_send_datagram(Lib& lib, Net& net, s32 soc, const std::array<u8, 0x22>& to, std::vector<u8> bytes)
{
    rudp::Packet p;
    p.soc = soc;
    std::memcpy(p.to.data(), to.data(), 16);
    p.bytes = std::move(bytes);
    if (!lib.has_filter.load(std::memory_order_acquire)) return net_send_one(lib, net, p);
    rudp::SendFilter filter;
    {
        std::lock_guard lock(lib.filter_m);
        filter = lib.filter;
    }
    std::vector<rudp::Packet> now;
    if (filter) filter(std::move(p), now);
    else now.push_back(std::move(p));
    s32 r = 0;
    for (const rudp::Packet& q : now) r = net_send_one(lib, net, q);
    return r;
}

void put16(std::vector<u8>& v, usize at, u16 x)
{
    v[at] = static_cast<u8>(x >> 8);
    v[at + 1] = static_cast<u8>(x);
}

// A packet sent at once, not aggregated (a listener's cookie).
void net_send_control(Lib& lib, Net& net, u8 mux, const Header& h, s32 soc, u16 src, const Addr& to)
{
    std::vector<u8> d = {0, 0};
    u16 prefix = 0x4000;
    if (mux == 1) {
        prefix = 0x8000;
        d.push_back(static_cast<u8>(src >> 8));
        d.push_back(static_cast<u8>(src));
    } else if (mux == 2) {
        prefix = 0xC000;
        d.push_back(static_cast<u8>(src >> 8));
        d.push_back(static_cast<u8>(src));
        d.push_back(static_cast<u8>(to.vport >> 8));
        d.push_back(static_cast<u8>(to.vport));
    } else if (mux) {
        return;
    }
    rudp::encode(h, d);
    if (d.size() > 70) return; // librudp builds these in 70 bytes
    put16(d, 0, static_cast<u16>(prefix | d.size()));
    net_send_datagram(lib, net, soc, to.raw, std::move(d));
}

// ------------------------------------------------------------------------------------------------- the aggregator

void agg_recompute(Agg& a, AggDest& d)
{
    u32 mx = 30, mn = 500;
    if (!d.users.empty()) {
        mn = 1000;
        for (const AggUser& u : d.users) {
            if (u.aggregation < mn) {
                mn = u.aggregation;
                if (u.aggregation < 30) mn = 30;
            }
            if (mx < u.aggregation) {
                mx = u.aggregation;
                if (u.aggregation > 999) mx = 1000;
            }
        }
        mn >>= 1;
    }
    d.max_wait = mx;
    a.interval_ms = mn;
}

s32 net_register_peer(Lib& lib, Net& net, Ctx& x)
{
    AggKey key;
    key.soc = x.soc;
    key.addr = x.peer.raw;
    if (key.soc < 0) return err(CELL_RUDP_ERROR_INVALID_SOCKET);
    Agg& a = net.agg;
    auto it = a.dests.find(key);
    if (it == a.dests.end()) {
        AggDest d;
        d.mem_obj = mem_alloc(lib, 0x238);
        if (!d.mem_obj) return err(CELL_RUDP_ERROR_MEMORY);
        d.mem_buf = mem_alloc(lib, a.mss);
        if (!d.mem_buf) {
            mem_free(lib, d.mem_obj);
            return err(CELL_RUDP_ERROR_MEMORY);
        }
        d.mem_node = mem_alloc(lib, 0x44);
        if (!d.mem_node) {
            mem_free(lib, d.mem_buf);
            mem_free(lib, d.mem_obj);
            return err(CELL_RUDP_ERROR_MEMORY);
        }
        d.cap = a.mss;
        it = a.dests.emplace(key, std::move(d));
    }
    AggDest& d = it->second;
    AggUser u;
    u.ctx = x.id;
    u.aggregation = x.c.aggregation;
    u.mem = mem_alloc(lib, 0xC);
    s32 r = 0;
    if (!u.mem) {
        r = err(CELL_RUDP_ERROR_MEMORY);
    } else {
        d.users.push_front(u);
        agg_recompute(a, d);
        if (!a.armed) {
            a.armed = true;
            d.wait = d.max_wait;
            r = sched_arm(lib, net.sched, a.target, u64{a.interval_ms} * 1000);
        }
    }
    if (r == err(CELL_RUDP_ERROR_MEMORY) && d.users.empty()) {
        mem_free(lib, d.mem_buf);
        mem_free(lib, d.mem_obj);
        mem_free(lib, d.mem_node);
        a.dests.erase(it);
    }
    return r;
}

void net_unregister_peer(Lib& lib, Net& net, Ctx& x)
{
    AggKey key;
    key.soc = x.soc;
    key.addr = x.peer.raw;
    if (key.soc < 0) return;
    Agg& a = net.agg;
    const auto it = a.dests.find(key);
    if (it == a.dests.end()) return;
    AggDest& d = it->second;
    if (!d.users.empty()) {
        for (auto u = d.users.begin(); u != d.users.end(); ++u) {
            if (u->ctx == x.id) {
                mem_free(lib, u->mem);
                d.users.erase(u);
                break;
            }
        }
        if (!d.users.empty()) return agg_recompute(a, d);
    }
    mem_free(lib, d.mem_buf);
    mem_free(lib, d.mem_obj);
    mem_free(lib, d.mem_node);
    a.dests.erase(it);
    if (a.dests.empty() && a.armed) {
        a.armed = false;
        sched_disarm(net.sched, a.target);
    }
}

// The datagram goes: each acknowledgement's delay grows by the time it waited, each segment's first-sending time
// becomes now.
s32 agg_flush(Lib& lib, Net& net, const AggKey& key, AggDest& d, u64 now)
{
    const u32 now_ms = static_cast<u32>(now / 1000);
    usize at = 0;
    for (const AggPacket& p : d.packets) {
        if (at + 2 > d.bytes.size()) break;
        const u16 prefix = static_cast<u16>(d.bytes[at] << 8 | d.bytes[at + 1]);
        const usize hdr = at + 2u * (prefix >> 14);
        if (hdr + 8 <= d.bytes.size() && (d.bytes[hdr] & 0x24) == 0x24) {
            const u16 delay = static_cast<u16>(d.bytes[hdr + 6] << 8 | d.bytes[hdr + 7]);
            put16(d.bytes, hdr + 6, static_cast<u16>(delay + (now_ms - p.queued_ms)));
        }
        if (p.stamp) p.stamp->stamp = now_ms;
        at += prefix & 0x7FF;
    }
    const s32 r = net_send_datagram(lib, net, key.soc, key.addr, d.bytes);
    d.bytes.clear();
    d.packets.clear();
    d.wait = d.max_wait;
    d.start = now;
    return r;
}

// A packet into the destination's datagram, written piece by piece as librudp writes it: the prefix's place, the
// vports, the header, the payload, each only when it fits. A piece that does not fit ends the packet there with what
// was written kept and the prefix left 0 (librudp's: it goes out so with the datagram; the segment is not sent). A
// header that does not fit is dropped whole (librudp keeps the fields that fitted).
bool agg_append(AggDest& d, u8 mux, u16 src, u16 dst, const Header& h, const u8* payload, u32 len, u32 now_ms,
    Seg* stamp)
{
    std::vector<u8>& v = d.bytes;
    const usize start = v.size();
    const auto fits = [&](usize n) { return v.size() + n <= d.cap; };
    const auto put = [&](u16 x) {
        if (!fits(2)) return false;
        v.push_back(static_cast<u8>(x >> 8));
        v.push_back(static_cast<u8>(x));
        return true;
    };
    if (!put(0)) return false;
    u16 prefix = 0x4000;
    if (mux == 1) {
        prefix = 0x8000;
        if (!put(src)) return false;
    } else if (mux == 2) {
        prefix = 0xC000;
        if (!put(src) || !put(dst)) return false;
    } else if (mux) {
        return false;
    }
    const usize header = v.size();
    rudp::encode(h, v);
    if (v.size() > d.cap) {
        v.resize(header);
        return false;
    }
    if (!fits(len)) return false;
    v.insert(v.end(), payload, payload + len);
    put16(v, start, static_cast<u16>(prefix | (v.size() - start)));
    d.packets.push_back({now_ms, stamp});
    return true;
}

// A context's packet to its peer: into the destination's datagram, sent at once when it may not wait.
void agg_send(Lib& lib, Net& net, const AggKey& key, s32, u8 mux, u16 src, u16 dst, const Header& h,
    const u8* payload, u32 len, u32 agg_ms, Seg* stamp)
{
    if (key.soc < 0) return;
    const auto it = net.agg.dests.find(key);
    if (it == net.agg.dests.end()) return;
    AggDest& d = it->second;
    const u64 now = now_us();
    bool flush_now = false;
    if (d.bytes.empty()) {
        d.start = now;
        d.wait = d.max_wait;
    } else if (d.cap - d.bytes.size() < 2u * mux + len + 0x42) {
        if (agg_flush(lib, net, key, d, now) < 0) return;
    }
    const u32 elapsed = static_cast<u32>((now - d.start) / 1000);
    if (elapsed < d.wait) {
        if (elapsed + agg_ms < d.wait) d.wait = elapsed + agg_ms;
        flush_now = agg_ms == 0;
    } else {
        flush_now = true;
    }
    if (agg_append(d, mux, src, dst, h, payload, len, static_cast<u32>(now / 1000), stamp) &&
        (d.packets.size() > 63 || flush_now))
        agg_flush(lib, net, key, d, now);
}

// The segments a context lets go are no longer the datagram's to stamp.
void agg_forget_stamps(Net& net, const AggKey& key, u16 vport)
{
    if (key.soc < 0) return;
    const auto it = net.agg.dests.find(key);
    if (it == net.agg.dests.end() || it->second.packets.empty()) return;
    AggDest& d = it->second;
    usize at = 0;
    for (AggPacket& p : d.packets) {
        if (at + 2 > d.bytes.size()) break;
        const u16 prefix = static_cast<u16>(d.bytes[at] << 8 | d.bytes[at + 1]);
        const u32 code = prefix >> 14;
        if (code == 1) p.stamp = nullptr;
        else if (code && at + 4 <= d.bytes.size() && vport == static_cast<u16>(d.bytes[at + 2] << 8 | d.bytes[at + 3]))
            p.stamp = nullptr;
        at += prefix & 0x7FF;
    }
}

void agg_on_timer(Lib& lib, Net& net, u64& interval)
{
    const u64 now = now_us();
    for (auto& [key, d] : net.agg.dests) {
        if (d.bytes.empty()) {
            d.packets.clear();
            continue;
        }
        if (now <= u64{d.wait} * 1000 + d.start) continue;
        agg_flush(lib, net, key, d, now);
    }
    interval = u64{net.agg.interval_ms} * 1000;
}

// ------------------------------------------------------------------------------------------------- datagrams in

void net_on_event(Lib&, Net& net, const Event& e)
{
    if (e.type != kQSocketReleased) return;
    const s32 soc = static_cast<s32>(e.arg);
    if (net.sockets.count(soc) || !net.handler) return;
    call_game(net.handler, {kEvSocketReleased, sext(soc), 0, 0, 0, 0, net.handler_arg}, "the event handler");
}

void net_socket_error(Lib& lib, Net& net, s32 soc)
{
    const auto it = net.sockets.find(soc);
    if (it == net.sockets.end()) return;
    for (auto& [p, n] : it->second->ctxs) ctx_post(*n.ctx, kQBadSocket);
    (void)lib;
}

// A packet for a context (`kind`: the mux mode it came in; 2 knows the sender's address and vports).
s32 ctx_receive(Lib& lib, Ctx& x, Bytes pkt, u8 kind, u16 dst, u16 src, const std::array<u8, 0x22>* from)
{
    if (x.flags & kXListening) {
        if (x.flags & kXTerminated) return 0;
    } else if (x.c.state == kIdle) {
        return 0;
    }
    if (kind < 2) return conn_input(lib, x.c, pkt);
    if (kind != 2 || !src || !from || !(*from)[0]) return 0;
    if (!(x.flags & kXListening)) {
        if (!(x.flags & kXPeer) || (x.c.state != kClosed && !addr_equal(x.peer.raw, *from))) return 0;
        const s32 r = conn_input(lib, x.c, pkt);
        if (!r && !(x.flags & kXPeer)) ctx_set_peer(lib, x, *from, src);
        return r;
    }
    Addr key;
    key.raw = *from;
    key.vport = src;
    if (const auto it = x.accepted.find(key); it != x.accepted.end())
        return ctx_receive(lib, *it->second.ctx, pkt, 2, dst, src, from);
    for (Ctx::Pending& p : x.pending)
        if (addr_equal(p.ctx->peer.raw, key.raw) && p.ctx->peer.vport == src)
            return ctx_receive(lib, *p.ctx, pkt, 2, dst, src, from);
    Header h;
    const std::optional<u32> used = rudp::decode(pkt.p + pkt.off, pkt.len, h);
    if (!used || h.type != rudp::kTypeSyn) return 0;
    if (x.pending.size() >= x.backlog) return 0;
    const auto queue_cookie = [&] {
        if (!x.cookies) return;
        Ctx::CookieRing& r = *x.cookies;
        if (r.tail == r.head && r.full) return;
        r.full = true;
        r.entries[r.tail] = key;
        if (++r.tail == r.entries.size()) r.tail = 0;
        ctx_post(x, kQCookies);
    };
    if (x.cookie_life) {
        if (!ctx_cookie_ring(lib, x)) return err(CELL_RUDP_ERROR_MEMORY);
        if (!(h.opts & rudp::kOptCookie)) {
            queue_cookie();
            return 0;
        }
        if (h.cookie_len != 32) return 0;
        const u32 v = check_cookie(x, h.cookie.data());
        if (v == 2) {
            if (x.pending.size() < x.backlog) queue_cookie();
            return 0;
        }
        if (v) return 0;
    }
    // A child completes the handshake (an Activate with the listener's options) and waits to be accepted.
    Ctx* child = nullptr;
    if (const s32 r = ctx_create(lib, *x.net, child); r < 0) return r;
    child->child = true;
    child->parent = &x;
    Conn& cc = child->c;
    const Conn& lc = x.c;
    const auto set_payload = [&](u32 v) {
        if (cc.state == kIdle && v >= kPayloadSlack && v <= std::min(cc.sndbuf, cc.rcvbuf)) {
            cc.segsize = v;
            cc.pool.bufsize = static_cast<u16>(v - kPayloadSlack);
        }
    };
    set_payload(lc.pool.bufsize + kPayloadSlack);
    if (cc.state == kIdle && lc.sndbuf >= cc.pool.bufsize + kPayloadSlack) cc.sndbuf = lc.sndbuf;
    if (cc.state == kIdle && lc.rcvbuf >= cc.pool.bufsize + kPayloadSlack) cc.rcvbuf = lc.rcvbuf;
    cc.conn_timeout = lc.conn_timeout;
    cc.close_wait_timeout = lc.close_wait_timeout;
    cc.aggregation = lc.aggregation;
    if (child->blocking) {
        child->wake |= kWakeCancel;
        child->blocking = false;
        child->cond.signal_all();
    }
    const u32 keep = kCStream | kCDelivery | kCOrder | kCNodelay | kCSack | kCOpt255;
    cc.flags = (cc.flags & ~keep) | (lc.flags & keep);
    if (lc.ka_interval != cc.ka_interval) {
        cc.ka_next = cc.ka_first = cc.ka_last = cc.ka_heard = 0;
        cc.ka_count = 0;
        cc.ka_interval = lc.ka_interval;
    }
    cc.ka_timeout = lc.ka_timeout;
    child->soc = x.soc;
    child->vport = x.vport;
    child->mux = x.mux;
    ctx_set_peer(lib, *child, *from, src);
    child->c.owner = child;
    conn_activate(lib, child->c);
    child->error = 0;
    if (x.blocking && !child->blocking) child->blocking = true;
    if (!ctx_receive(lib, *child, pkt, 2, dst, src, from)) {
        Ctx::Pending p;
        p.since = now_us();
        p.ctx = child;
        p.mem = mem_alloc(lib, 0x28);
        if (p.mem) x.pending.push_back(p);
    } else {
        ctx_delete(lib, *x.net, child->id);
    }
    return 0;
}

// A datagram from a socket (the I/O thread's, or the game's through cellRudpNetReceived): its packets to their
// contexts. MSG_MALFORMED when a packet does not parse or finds no context.
s32 net_receive(Lib& lib, Net& net, s32 soc, const u8* data, u32 len, const std::array<u8, 0x22>& from, bool api)
{
    s32 result = 0;
    if (api && net.io_enabled) {
        result = err(CELL_RUDP_ERROR_THREAD_IN_USE);
    } else {
        const auto rit = net.sockets.find(soc);
        if (rit == net.sockets.end()) {
            result = err(CELL_RUDP_ERROR_INVALID_SOCKET);
        } else if (!rit->second->ctxs.empty()) {
            SockRec& rec = *rit->second;
            u32 at = 0;
            while (len - at > 1) {
                const u16 prefix = static_cast<u16>(data[at] << 8 | data[at + 1]);
                at += 2;
                const u32 code = prefix >> 14, plen = prefix & 0x7FF;
                if (!code || plen < 2 * code || plen - 2 > len - at) {
                    result = err(CELL_RUDP_ERROR_MSG_MALFORMED);
                    break;
                }
                Addr key;
                std::memcpy(key.raw.data(), from.data(), 0x22);
                key.vport = 0;
                u16 dst = 0;
                const u32 pkt_len = plen - 2 * code;
                const u8 mux = static_cast<u8>(code - 1);
                bool skip = false;
                if (mux == 1) {
                    if (len - at <= 1) {
                        result = err(CELL_RUDP_ERROR_MSG_MALFORMED);
                        break;
                    }
                    key.vport = static_cast<u16>(data[at] << 8 | data[at + 1]);
                    at += 2;
                } else if (mux == 2) {
                    if (len - at <= 3) {
                        result = err(CELL_RUDP_ERROR_MSG_MALFORMED);
                        break;
                    }
                    key.vport = static_cast<u16>(data[at] << 8 | data[at + 1]);
                    dst = static_cast<u16>(data[at + 2] << 8 | data[at + 3]);
                    at += 4;
                }
                Ctx* x = nullptr;
                if (mux > 2) skip = true;
                if (!skip) {
                    const u8 m = rec.ctxs.begin()->first->mux;
                    if (m != 0xFF && m != mux) skip = true;
                }
                if (!skip && mux == 2) {
                    const auto v = rec.vports.find(dst);
                    if (v == rec.vports.end()) skip = true;
                    else x = v->second.ctx;
                } else if (!skip) {
                    const auto p = rec.peers.find(key);
                    if (p == rec.peers.end()) skip = true;
                    else {
                        x = p->second.ctx;
                        if (mux == 1 && (key.vport != x->vport || x->peer.vport != key.vport)) skip = true;
                    }
                }
                if (!skip && len - at < pkt_len) {
                    result = err(CELL_RUDP_ERROR_MSG_MALFORMED);
                    break;
                }
                if (skip) { // nobody's packet: passed over (malformed only when it runs past the datagram)
                    if (len - at < pkt_len) {
                        result = err(CELL_RUDP_ERROR_MSG_MALFORMED);
                        break;
                    }
                    at += pkt_len;
                    continue;
                }
                Bytes pkt;
                pkt.p = const_cast<u8*>(data);
                pkt.off = at;
                pkt.len = pkt_len;
                pkt.cap = at + pkt_len;
                result = ctx_receive(lib, *x, pkt, mux, dst, key.vport, &from);
                if (result < 0) break;
                at += pkt_len;
            }
        }
    }
    st_add32(lib, kStRcvdUdpPackets, 1);
    st_add64(lib, kStRcvdUdpBytes, len);
    return result;
}

// ------------------------------------------------------------------------------------------------- sockets

void io_add_socket(Net& net, s32 soc);
void io_remove_socket(Net& net, s32 soc);

// A context bound to a socket: mux mode 1 (P2P: the peers share the vport, it is never 0) or 2 (the vport its own, 0
// the next free one from 10001). A new socket record when the socket has none; INVALID_MUXMODE when its contexts use
// the other mode.
s32 net_bind(Lib& lib, Net& net, s32 id, s32 soc, u16 vport, u8 mux)
{
    Ctx* x = ctx_find(net, id, false, nullptr);
    if (!x) return err(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    if (api_state(*x)) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    if (x->soc != -1) return err(CELL_RUDP_ERROR_ALREADY_BOUND);
    bool fresh = false;
    auto rit = net.sockets.find(soc);
    if (rit == net.sockets.end()) {
        auto rec = std::make_unique<SockRec>();
        rec->mem = mem_alloc(lib, 0x84);
        if (!rec->mem) return err(CELL_RUDP_ERROR_MEMORY);
        rec->soc = soc;
        rit = net.sockets.emplace(soc, std::move(rec));
        fresh = true;
    } else if (!rit->second->ctxs.empty()) {
        const u8 m = rit->second->ctxs.begin()->first->mux;
        if (m != 0xFF && m != mux) return err(CELL_RUDP_ERROR_INVALID_MUXMODE);
    }
    SockRec& rec = *rit->second;
    s32 r = 0;
    bool link = false;
    if (mux == 1) {
        if (!vport) r = err(CELL_RUDP_ERROR_INVALID_VPORT);
        else link = true;
    } else if (mux == 0) {
        link = true;
    } else if (mux == 2) {
        if (!vport) {
            u16 v = rec.next_vport;
            for (;;) {
                if (!++v) v = 10000;
                if (rec.next_vport == v) {
                    r = err(CELL_RUDP_ERROR_VPORT_EXHAUSTED);
                    break;
                }
                if (!rec.vports.count(v)) break;
            }
            if (!r) {
                rec.next_vport = v;
                vport = v;
            }
        }
        if (!r) {
            if (rec.vports.count(vport)) {
                r = err(CELL_RUDP_ERROR_VPORT_IN_USE);
            } else {
                SockRec::VportNode n;
                n.ctx = x;
                n.mem = mem_alloc(lib, 0x1C);
                if (!n.mem) r = err(CELL_RUDP_ERROR_MEMORY);
                else {
                    rec.vports.emplace(vport, n);
                    link = true;
                }
            }
        }
    } else {
        r = err(CELL_RUDP_ERROR_INVALID_MUXMODE);
    }
    if (link) {
        if (api_state(*x) || x->c.state) r = err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
        else if (x->soc != -1) r = err(CELL_RUDP_ERROR_ALREADY_BOUND);
        else {
            x->mux = mux;
            x->soc = soc;
            x->vport = vport;
            SockRec::CtxNode n;
            n.ctx = x;
            n.mem = mem_alloc(lib, 0x20);
            if (!n.mem) {
                r = err(CELL_RUDP_ERROR_MEMORY);
            } else {
                rec.ctxs.emplace(x, n);
                if (net.io_enabled && fresh) io_add_socket(net, soc);
            }
        }
    }
    if (r < 0 && fresh) {
        auto again = net.sockets.find(soc);
        if (again != net.sockets.end()) {
            mem_free(lib, again->second->mem);
            net.sockets.erase(again);
        }
    }
    return r;
}

// Connect's preparation (Initiate, Activate, Listen): the socket bound, the peer known, the context findable by the
// packets that will come.
s32 net_prepare(Lib& lib, Net& net, Ctx& x, const std::array<u8, 0x22>* addr, u16 vport)
{
    const u32 st = api_state(x);
    if (st > 1 && st != kListening) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    const auto rit = x.soc == -1 ? net.sockets.end() : net.sockets.find(x.soc);
    if (rit == net.sockets.end()) return err(CELL_RUDP_ERROR_NOT_BOUND);
    SockRec& rec = *rit->second;
    if (addr) {
        if (const s32 r = ctx_set_peer(lib, x, *addr, vport); r < 0) return r;
    }
    if (x.mux < 2) {
        if (x.mux == 1 && !x.vport) return err(CELL_RUDP_ERROR_INVALID_VPORT);
        Addr key;
        key.raw = x.peer.raw;
        key.vport = x.vport;
        const auto p = rec.peers.find(key);
        if (p == rec.peers.end()) {
            SockRec::PeerNode n;
            n.ctx = &x;
            n.mem = mem_alloc(lib, 0x40);
            if (n.mem) rec.peers.emplace(key, n);
        } else if (p->second.ctx != &x) {
            p->second.ctx = &x;
        }
    } else if (x.mux == 2) {
        if (!x.vport) return err(CELL_RUDP_ERROR_INVALID_VPORT);
        if (!rec.vports.count(x.vport)) return err(CELL_RUDP_ERROR_NOT_BOUND);
    } else {
        return err(CELL_RUDP_ERROR_INVALID_MUXMODE);
    }
    return 0;
}

// A context leaves its poll set and its socket; the socket's last context releases it (SOCKET_RELEASED, queued).
void net_ctx_removed(Lib& lib, Net& net, Ctx& x)
{
    if (x.poll >= 0) {
        if (PollSet* p = poll_find(net, x.poll)) {
            const auto it = p->entries.find(x.id);
            if (it != p->entries.end()) {
                if (!(it->second.ret & 0x8000) && p->push(x.id)) p->cond.signal_all();
                mem_free(lib, it->second.mem);
                p->entries.erase(it);
            }
        }
        x.poll = -1;
    }
    const auto rit = x.soc < 0 ? net.sockets.end() : net.sockets.find(x.soc);
    if (rit != net.sockets.end()) {
        SockRec& rec = *rit->second;
        if (x.mux == 2) {
            const auto v = rec.vports.find(x.vport);
            if (v != rec.vports.end() && v->second.ctx == &x) {
                mem_free(lib, v->second.mem);
                rec.vports.erase(v);
            }
        } else {
            Addr key;
            key.raw = x.peer.raw;
            key.vport = x.peer.vport;
            const auto p = rec.peers.find(key);
            if (p != rec.peers.end() && p->second.ctx == &x) {
                mem_free(lib, p->second.mem);
                rec.peers.erase(p);
            }
        }
        const auto n = rec.ctxs.find(&x);
        if (n != rec.ctxs.end()) {
            mem_free(lib, n->second.mem);
            rec.ctxs.erase(n);
        }
        if (rec.ctxs.empty()) {
            const s32 soc = x.soc;
            sched_post(lib, net, {net.target, kQSocketReleased, 0, static_cast<u32>(soc)});
            mem_free(lib, rec.mem);
            net.sockets.erase(rit);
            if (net.io_enabled) io_remove_socket(net, soc);
        }
    }
    x.handler = 0;
    x.arg = 0;
}

// ------------------------------------------------------------------------------------------------- the I/O thread

void io_wake(Net& net)
{
    IoThread& io = net.io;
    if (!io.running || !t_ppu) return;
    if (io.woken.exchange(1)) return;
    u8* base = guest_base(*t_ppu);
    Scratch s(*t_ppu, 32);
    std::memcpy(base + s.addr, io.wake_addr.data(), 16);
    libnet("sendto", {sext(io.wake_soc), s.addr + 16u, 4, 0, s.addr, 16});
}

void io_add_socket(Net& net, s32 soc)
{
    IoThread& io = net.io;
    if (!io.running) return;
    if (std::find(io.sockets.begin(), io.sockets.end(), soc) == io.sockets.end()) io.sockets.push_back(soc);
    io_wake(net);
}

void io_remove_socket(Net& net, s32 soc)
{
    IoThread& io = net.io;
    if (!io.running) return;
    io.sockets.erase(std::remove(io.sockets.begin(), io.sockets.end(), soc), io.sockets.end());
    io_wake(net);
}

// The internal I/O thread: socketselect over its sockets (16.7 ms at most), each datagram to the library (the event
// handler hears event 101 first), then the scheduler. A socket whose datagram the library refuses, or whose recvfrom
// fails, is no longer read (librudp's).
void _pspocket_rudp_io_thread(PPUContext& ctx, u64)
{
    OnThread on(ctx);
    Lib& lib = lib_of();
    u8* base = guest_base(ctx);
    u32 bufsize = kIoBufferDefault;
    {
        std::lock_guard lock(lib.lock);
        if (lib.net) bufsize = lib.net->io.bufsize;
    }
    const u32 block = HleProcess::current()->main_memory().alloc(bufsize + 512, 16);
    if (!block) PS_FATAL(kChan, "no guest memory for the internal I/O thread's buffers");
    const u32 rset = block, tv = block + 128, from = block + 160, fromlen = block + 176, buf = block + 512;
    for (;;) {
        std::vector<s32> sockets;
        s32 wake = -1;
        {
            std::lock_guard lock(lib.lock);
            if (!lib.net || !lib.net->io.running || lib.net->io.stop.load()) break;
            lib.net->io.woken.store(0);
            sockets = lib.net->io.sockets;
            wake = lib.net->io.wake_soc;
        }
        std::vector<s32> all = {wake};
        all.insert(all.end(), sockets.begin(), sockets.end());
        std::memset(base + rset, 0, 128);
        s32 nfds = 0;
        for (const s32 s : all) {
            if (s < 0 || s >= 1024) continue;
            ps_write32(base, rset + 4 * (s >> 5), ps_read32(base, rset + 4 * (s >> 5)) | 1u << (s & 31));
            nfds = std::max(nfds, s + 1);
        }
        ps_write64(base, tv, kIoPollUs / 1000000);
        ps_write64(base, tv + 8, kIoPollUs % 1000000);
        const s32 n = libnet("socketselect", {sext(nfds), rset, 0, 0, tv});
        std::vector<s32> ready, bad;
        if (n > 0) {
            for (const s32 s : all) {
                if (s >= 0 && s < 1024 && (ps_read32(base, rset + 4 * (s >> 5)) & 1u << (s & 31))) ready.push_back(s);
                if (ready.size() >= 8) break;
            }
        } else if (n < 0 && net_errno() == kNetEbadf) { // a socket was closed under its contexts
            for (const s32 s : sockets) {
                StackVar opt(ctx, base, 8);
                ps_write32(base, opt.addr() + 4, 4);
                if (libnet("getsockopt", {sext(s), kSysNetSolSocket, 0x1007, opt.addr(), opt.addr() + 4}) < 0 &&
                    net_errno() == kNetEbadf) {
                    bad.push_back(s);
                    break;
                }
            }
        }
        for (const s32 s : bad) {
            std::lock_guard lock(lib.lock);
            if (lib.net) lib.net->io.sockets.erase(std::remove(lib.net->io.sockets.begin(),
                                                       lib.net->io.sockets.end(), s),
                lib.net->io.sockets.end());
        }
        for (const s32 s : ready) {
            ps_write32(base, fromlen, 16);
            std::memset(base + from, 0, 16);
            const s32 r = libnet("recvfrom", {sext(s), buf, bufsize, 0, from, fromlen});
            const s32 e = r < 0 ? net_errno() : 0;
            if (s == wake) continue;
            std::lock_guard lock(lib.lock);
            if (!lib.net || !lib.net->io.running) break;
            Net& net = *lib.net;
            if (r >= 0) {
                if (net.handler) {
                    const std::optional<u64> h = call_game(net.handler,
                        {kEvBeforeReceive, sext(s), buf, static_cast<u32>(r), from, ps_read32(base, fromlen),
                            net.handler_arg},
                        "the event handler");
                    if (h && static_cast<s32>(*h) < 0) continue;
                }
                std::array<u8, 0x22> key{};
                std::memcpy(key.data(), base + from, 16);
                std::vector<u8> bytes(base + buf, base + buf + r);
                if (net_receive(lib, net, s, bytes.data(), static_cast<u32>(r), key, false) < 0)
                    net.io.sockets.erase(std::remove(net.io.sockets.begin(), net.io.sockets.end(), s),
                        net.io.sockets.end());
            } else if (e != kNetEwouldblock && e != 54) { // ECONNRESET
                net.io.sockets.erase(std::remove(net.io.sockets.begin(), net.io.sockets.end(), s), net.io.sockets.end());
            }
        }
        std::lock_guard lock(lib.lock);
        if (!lib.net || !lib.net->io.running || lib.net->io.stop.load()) break;
        lib.net->io.woken.store(0);
        sched_run(lib, *lib.net, 0);
    }
    HleProcess::current()->main_memory().free(block);
    libnet("sys_net_free_thread_context", {0, 1}); // the thread's libnet context, as librudp's threads end
    const Function* exit = registry().find("sysPrxForUser", recomp::nid_of("sys_ppu_thread_exit"));
    if (!exit) PS_FATAL(kChan, "sysPrxForUser::sys_ppu_thread_exit, which the I/O thread ends with, is not pspocket's");
    ctx.gpr[3] = 0;
    exit->fn(ctx, guest_base(ctx));
}

// Starts the internal I/O thread: its buffer, a wake-up socket (UDP at 127.0.0.1, non-blocking), the PPU thread
// ("librudp", joinable).
s32 io_start(Lib& lib, Net& net)
{
    IoThread& io = net.io;
    io.buf_mem = mem_alloc(lib, io.bufsize);
    if (!io.buf_mem) return err(CELL_RUDP_ERROR_MEMORY);
    PPUContext& ctx = *t_ppu;
    u8* base = guest_base(ctx);
    StackVar v(ctx, base, 0x40);
    const s32 soc = libnet("socket", {2, 2, 0});
    if (soc < 0) return err(CELL_RUDP_ERROR_INVALID_SOCKET);
    std::memset(base + v.addr(), 0, 16);
    ps_write8(base, v.addr() + 1, 2);
    ps_write32(base, v.addr() + 4, 0x7F000001);
    libnet("bind", {sext(soc), v.addr(), 16});
    ps_write32(base, v.addr() + 0x20, 16);
    const auto fail = [&](u32 code) {
        libnet("socketclose", {sext(soc)});
        return err(code);
    };
    if (libnet("getsockname", {sext(soc), v.addr(), v.addr() + 0x20}) < 0) return fail(CELL_RUDP_ERROR_INVALID_SOCKET);
    std::memcpy(io.wake_addr.data(), base + v.addr(), 16);
    ps_write32(base, v.addr() + 0x24, 1);
    if (libnet("setsockopt", {sext(soc), kSysNetSolSocket, kSysNetSoNbio, v.addr() + 0x24, 4}) < 0)
        return fail(CELL_RUDP_ERROR_INVALID_SOCKET);
    io.wake_soc = soc;
    io.sockets.clear();
    io.stop.store(false);
    io.running = true;
    const u32 entry = HleProcess::current()->link_function(kModule, recomp::nid_of("_pspocket_rudp_io_thread"),
        "_pspocket_rudp_io_thread");
    const Function* create = registry().find("sysPrxForUser", recomp::nid_of("sys_ppu_thread_create"));
    if (!entry || !create) {
        io.running = false;
        io.wake_soc = -1;
        return fail(CELL_RUDP_ERROR_THREAD);
    }
    std::memcpy(base + v.addr() + 0x28, "librudp", 8);
    u64 saved[7];
    for (u32 k = 0; k < 7; k++) saved[k] = ctx.gpr[3 + k];
    const u64 args[7] = {v.addr() + 0x30u, entry, 0, io.priority, io.stack, 1, v.addr() + 0x28u};
    for (u32 k = 0; k < 7; k++) ctx.gpr[3 + k] = args[k];
    create->fn(ctx, base);
    const s32 r = static_cast<s32>(ctx.gpr[3]);
    for (u32 k = 0; k < 7; k++) ctx.gpr[3 + k] = saved[k];
    if (r != lv2::CELL_OK) {
        io.running = false;
        io.wake_soc = -1;
        PS_LOG_WARN(kChan, "cellRudpEnableInternalIOThread: sys_ppu_thread_create gives {:#x}", static_cast<u32>(r));
        return fail(CELL_RUDP_ERROR_THREAD);
    }
    io.thread_id = static_cast<u32>(ps_read64(base, v.addr() + 0x30));
    io.started = true;
    return 0;
}

// Stops the internal I/O thread (the library's lock released while it is joined).
void io_stop(Lib& lib, Net& net)
{
    IoThread& io = net.io;
    if (!io.running) return;
    io.running = false;
    io.stop.store(true);
    io.woken.store(0);
    {
        u8* base = guest_base(*t_ppu);
        Scratch s(*t_ppu, 32);
        std::memcpy(base + s.addr, io.wake_addr.data(), 16);
        libnet("sendto", {sext(io.wake_soc), s.addr + 16u, 4, 0, s.addr, 16});
    }
    const u32 tid = io.thread_id;
    const bool self = lv2::sys_ppu_thread_get_id(thread_of(*t_ppu)) == tid;
    if (!self && tid) {
        const u32 depth = lib.lock.release_all();
        StackVar status(*t_ppu, guest_base(*t_ppu), 16);
        lv2::sys_ppu_thread_join(thread_of(*t_ppu), tid, status.addr());
        lib.lock.reacquire(depth);
    }
    if (io.wake_soc >= 0) libnet("socketclose", {sext(io.wake_soc)});
    io.wake_soc = -1;
    io.sockets.clear();
    mem_free(lib, io.buf_mem);
    io.started = false;
}

// ------------------------------------------------------------------------------------------------- life

std::unique_ptr<Net> net_create(Lib& lib, Block mem)
{
    auto net = std::make_unique<Net>();
    net->mem = mem;
    Sched& s = net->sched;
    s.ring_mem = mem_alloc(lib, 0xC00);
    s.table_mem = mem_alloc(lib, 0x100);
    Target agg;
    agg.kind = Target::Kind::Agg;
    if (sched_register(lib, s, agg, net->agg.target) < 0) net->agg.target = 0;
    if (s.table_mem) {
        Target t;
        t.kind = Target::Kind::Net;
        if (sched_register(lib, s, t, net->target) < 0) net->target = 0;
    }
    return net;
}

// Everything goes: the polls, the aggregator's datagrams (not sent), the sockets, the scheduler.
void net_destroy(Lib& lib, std::unique_ptr<Net> net)
{
    for (auto& [id, e] : net->polls) {
        poll_destroy(lib, *net, *e.first);
        mem_free(lib, e.first->mem_obj);
        mem_free(lib, e.second);
    }
    net->polls.clear();
    if (net->agg.target) sched_unregister(lib, net->sched, net->agg.target);
    for (auto& [k, d] : net->agg.dests) {
        for (AggUser& u : d.users) mem_free(lib, u.mem);
        mem_free(lib, d.mem_buf);
        mem_free(lib, d.mem_obj);
        mem_free(lib, d.mem_node);
    }
    net->agg.dests.clear();
    for (auto& [soc, rec] : net->sockets) {
        for (auto& [p, n] : rec->ctxs) mem_free(lib, n.mem);
        for (auto& [k, n] : rec->peers) mem_free(lib, n.mem);
        for (auto& [k, n] : rec->vports) mem_free(lib, n.mem);
        mem_free(lib, rec->mem);
    }
    net->sockets.clear();
    Sched& s = net->sched;
    for (auto& [due, node] : s.timers) mem_free(lib, node.mem);
    s.timers.clear();
    for (auto& [id, t] : s.targets) mem_free(lib, t.mem);
    s.targets.clear();
    mem_free(lib, s.table_mem);
    mem_free(lib, s.ring_mem);
    Block mem = net->mem;
    net.reset();
    mem_free(lib, mem);
}

// The library's lock and initialisation check around each call.
struct Locked {
    Lib& lib;
    OnThread on;
    explicit Locked(PPUContext& ctx) : lib(lib_of()), on(ctx) { lib.lock.lock(); }
    ~Locked() { lib.lock.unlock(); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
    bool ready() const { return lib.initialized && lib.net; }
};

bool writable_guest(PPUContext& ctx, u32 addr, u32 size)
{
    if (!addr) return false;
    lv2::PpuThread& t = thread_of(ctx);
    for (u32 at = 0; at < size; at += 0x1000)
        if (!guest_readable(t, addr + at, std::min<u32>(size - at, 0x1000))) return false;
    return true;
}

// A pointer the module would follow: one that is not guest memory is refused (librudp would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (writable_guest(ctx, addr, size)) return true;
    PS_LOG_ERROR(kChan, "{}: {:#x} ({} bytes) is not guest memory", what, addr, size);
    return false;
}

// ------------------------------------------------------------------------------------------------- the API: life

// int cellRudpInit(CellRudpAllocator *allocator): NULL for the library's own (the system's malloc).
s32 cellRudpInit(PPUContext& ctx, u32 allocator)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    while (lib.ending) {
        lib.lock.unlock();
        std::this_thread::yield();
        lib.lock.lock();
    }
    if (lib.initialized) return err(CELL_RUDP_ERROR_ALREADY_INITIALIZED);
    if (allocator && !usable(ctx, allocator, 8, "cellRudpInit: allocator")) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    lib.app_malloc = allocator ? ps_read32(guest_base(ctx), allocator) : 0;
    lib.app_free = allocator ? ps_read32(guest_base(ctx), allocator + 4) : 0;
    lib.mem_current = lib.mem_peak = lib.mem_allocs = lib.mem_frees = 0;
    lib.status_mem = mem_alloc(lib, 0x128);
    if (lib.status_mem) {
        lib.status = {};
        Block nm = mem_alloc(lib, 0x200);
        if (nm) {
            lib.net = net_create(lib, nm);
            Net& net = *lib.net;
            if (net.sched.table_mem && net.agg.target > 0 && net.target > 0) {
                lib.initialized = true;
                return 0;
            }
            net_destroy(lib, std::move(lib.net));
        }
        mem_free(lib, lib.status_mem);
    }
    lib.net.reset();
    return err(CELL_RUDP_ERROR_MEMORY);
}

// int cellRudpEnd(void): every waiter returns, the contexts go (nothing is told to the peers), the I/O thread stops.
s32 cellRudpEnd(PPUContext& ctx)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    lib.initialized = false;
    lib.ending = true;
    Net& net = *lib.net;
    lv2::PpuThread& t = thread_of(ctx);
    while (net.sched.busy > 0) {
        net.sched.cancelled = true;
        net.sched.cond.signal_all();
        net.sched.cond.wait(lib.lock, t, 0);
    }
    for (auto& [id, e] : net.polls) poll_cancel(lib, *e.first, false, false);
    for (auto& [id, e] : net.ctxs) {
        Ctx* x = e.first;
        while (x->waiters > 0) {
            x->wake |= kWakeCancel;
            x->cond.signal_all();
            x->cond.wait(lib.lock, t, 0);
        }
        x->wake &= ~kWakeCancel;
    }
    for (auto& [id, e] : net.ctxs) {
        Ctx* x = e.first;
        if (x->valid && x->parent) {
            ctx_detach_child(lib, *x->parent, x->peer);
            x->parent = nullptr;
        }
    }
    while (!net.ctxs.empty()) ctx_delete(lib, net, net.ctxs.begin()->first);
    io_stop(lib, net);
    net_destroy(lib, std::move(lib.net));
    mem_free(lib, lib.status_mem);
    lib.ending = false;
    return 0;
}

// int cellRudpEnableInternalIOThread(uint32_t stack_size, uint32_t priority): before any context (NOT_ACCEPTABLE
// after), once (THREAD_IN_USE).
s32 cellRudpEnableInternalIOThread(PPUContext& ctx, u32 stack_size, u32 priority)
{
    Locked l(ctx);
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (priority >= 0xC00) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    Net& net = *l.lib.net;
    if (!net.ctxs.empty()) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    if (net.io_enabled) return err(CELL_RUDP_ERROR_THREAD_IN_USE);
    net.io.stack = std::max<u32>(stack_size, 0x1000);
    net.io.priority = priority;
    if (const s32 r = io_start(l.lib, net); r < 0) return r;
    net.io_enabled = true;
    return 0;
}

// int cellRudpSetEventHandler(CellRudpEventHandler handler, void *arg)
s32 cellRudpSetEventHandler(PPUContext& ctx, u32 handler, u32 arg)
{
    Locked l(ctx);
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    l.lib.net->handler = handler;
    l.lib.net->handler_arg = arg;
    return 0;
}

// int cellRudpSetMaxSegmentSize(uint16_t mss): any value, only while there is no context (NOT_ACCEPTABLE).
s32 cellRudpSetMaxSegmentSize(PPUContext& ctx, u16 mss)
{
    Locked l(ctx);
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    Net& net = *l.lib.net;
    if (!net.ctxs.empty()) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    net.agg.mss = mss;
    net.io.bufsize = mss;
    return 0;
}

// int cellRudpGetMaxSegmentSize(uint16_t *mss)
s32 cellRudpGetMaxSegmentSize(PPUContext& ctx, u32 mss)
{
    Locked l(ctx);
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (!mss) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    if (!usable(ctx, mss, 2, "cellRudpGetMaxSegmentSize")) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    ps_write16(guest_base(ctx), mss, l.lib.net->agg.mss);
    return 0;
}

// int cellRudpGetStatus(CellRudpStatus *status, size_t size): `size` bytes (1 to 296) of the counters, then the
// contexts and the memory words written whatever the size (librudp's).
s32 cellRudpGetStatus(PPUContext& ctx, u32 status, u32 size)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (!status || size - 1 >= 0x128) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    if (!usable(ctx, status, std::max<u32>(size, 0x104), "cellRudpGetStatus")) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    u8* base = guest_base(ctx);
    std::memcpy(base + status, lib.status.data(), size);
    ps_write32(base, status + kStContexts, static_cast<u32>(lib.net->ctxs.size()));
    ps_write32(base, status + kStAllocs, lib.mem_allocs);
    ps_write32(base, status + kStFrees, lib.mem_frees);
    ps_write32(base, status + kStMemCurrent, lib.mem_current);
    ps_write32(base, status + kStMemPeak, lib.mem_peak);
    return 0;
}

// ------------------------------------------------------------------------------------------------- the API: contexts

// int cellRudpCreateContext(CellRudpContextEventHandler handler, void *arg, int *ctx_id)
s32 cellRudpCreateContext(PPUContext& ctx, u32 handler, u32 arg, u32 ctx_id)
{
    Locked l(ctx);
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (!ctx_id) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    if (!usable(ctx, ctx_id, 4, "cellRudpCreateContext")) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    u8* base = guest_base(ctx);
    ps_write32(base, ctx_id, 0xFFFFFFFF);
    Ctx* x = nullptr;
    if (const s32 r = ctx_create(l.lib, *l.lib.net, x); r < 0) return r;
    x->handler = handler;
    x->arg = arg;
    ps_write32(base, ctx_id, static_cast<u32>(x->id));
    return 0;
}

// The context of an API call (terminated ones are gone for it).
Ctx* api_ctx(Locked& l, s32 id, s32& r) { return ctx_find(*l.lib.net, id, false, &r); }

// int cellRudpSetOption(int ctx_id, int option, const void *optval, size_t optlen): 4 bytes; the connection's
// parameters only before a connection (NOT_ACCEPTABLE).
s32 cellRudpSetOption(PPUContext& ctx, s32 ctx_id, s32 option, u32 optval, u32 optlen)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0 || !optval) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    s32 r = 0;
    Ctx* x = api_ctx(l, ctx_id, r);
    if (!x) return r;
    Conn& c = x->c;
    const auto value = [&]() -> std::optional<u32> {
        if (optlen != 4) return std::nullopt;
        if (!usable(ctx, optval, 4, "cellRudpSetOption")) return std::nullopt;
        return ps_read32(guest_base(ctx), optval);
    };
    const bool idle = !api_state(*x);
    const auto bit = [&](u32 b, u32 v) {
        if (v) c.flags |= b;
        else c.flags &= ~b;
    };
    switch (option) {
    case 1: {
        if (!idle) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
        const auto v = value();
        if (!v) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        const u32 seg = *v + kPayloadSlack;
        if (seg > lib.net->agg.mss) return err(CELL_RUDP_ERROR_PAYLOAD_TOO_LARGE);
        if (c.state) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
        if (seg < kPayloadSlack || seg > std::min(c.sndbuf, c.rcvbuf)) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        c.segsize = seg;
        c.pool.bufsize = static_cast<u16>(seg - kPayloadSlack);
        return 0;
    }
    case 2:
    case 3: {
        if (!idle) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
        const auto v = value();
        if (!v) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        if (c.state) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
        if (*v < c.pool.bufsize + kPayloadSlack) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        (option == 2 ? c.sndbuf : c.rcvbuf) = *v;
        return 0;
    }
    case 4: {
        const auto v = value();
        if (!v) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        bit(kCNodelay, *v);
        return 0;
    }
    case 5:
    case 6:
    case 8:
    case 12: {
        if (!idle) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
        const auto v = value();
        if (!v) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        bit(option == 5 ? kCDelivery : option == 6 ? kCOrder : option == 8 ? kCStream : kCSack, *v);
        return 0;
    }
    case 7: {
        const auto v = value();
        if (!v) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        if (!x->blocking) {
            if (!*v) x->blocking = true;
        } else if (*v) {
            x->blocking = false;
            ctx_wake(*x, kWakeCancel);
        }
        return 0;
    }
    case 9:
    case 10:
    case 11:
    case 13: {
        if (!idle) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
        const auto v = value();
        if (!v) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        if (option == 9) c.conn_timeout = *v;
        else if (option == 10) c.close_wait_timeout = *v;
        else if (option == 11) c.aggregation = *v;
        else x->cookie_life = *v;
        return 0;
    }
    case 15:
    case 16:
    case 17:
    case 18:
    case 19: {
        const auto v = value();
        if (!v) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        if (option == 15) x->read_timeout = *v;
        else if (option == 16) x->write_timeout = *v;
        else if (option == 17) x->flush_timeout = *v;
        else if (option == 19) c.ka_timeout = *v;
        else if (c.ka_interval != *v) {
            c.ka_next = c.ka_first = c.ka_last = c.ka_heard = 0;
            c.ka_count = 0;
            c.ka_interval = *v;
        }
        return 0;
    }
    case 0xFF: {
        const auto v = value();
        if (!v) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        bit(kCOpt255, *v);
        return 0;
    }
    default: return err(CELL_RUDP_ERROR_INVALID_OPTION);
    }
}

// int cellRudpGetOption(int ctx_id, int option, void *optval, size_t optlen): 4 bytes; LAST_ERROR is cleared by its
// reading.
s32 cellRudpGetOption(PPUContext& ctx, s32 ctx_id, s32 option, u32 optval, u32 optlen)
{
    Locked l(ctx);
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0 || !optval) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    s32 r = 0;
    Ctx* x = api_ctx(l, ctx_id, r);
    if (!x) return r;
    if (static_cast<u32>(option) >= 20 || !option) return err(CELL_RUDP_ERROR_INVALID_OPTION);
    if (optlen != 4) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    if (!usable(ctx, optval, 4, "cellRudpGetOption")) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    const Conn& c = x->c;
    u32 v = 0;
    switch (option) {
    case 1: v = c.pool.bufsize; break;
    case 2: v = c.sndbuf; break;
    case 3: v = c.rcvbuf; break;
    case 4: v = (c.flags & kCNodelay) ? 1 : 0; break;
    case 5: v = c.delivery() ? 1 : 0; break;
    case 6: v = c.order() ? 1 : 0; break;
    case 7: v = x->blocking ? 0 : 1; break;
    case 8: v = c.stream() ? 1 : 0; break;
    case 9: v = c.conn_timeout; break;
    case 10: v = c.close_wait_timeout; break;
    case 11: v = c.aggregation; break;
    case 12: v = (c.flags & kCSack) ? 1 : 0; break;
    case 13: v = x->cookie_life; break;
    case 14:
        v = static_cast<u32>(x->error);
        x->error = 0;
        break;
    case 15: v = x->read_timeout; break;
    case 16: v = x->write_timeout; break;
    case 17: v = x->flush_timeout; break;
    case 18: v = c.ka_interval; break;
    case 19: v = c.ka_timeout; break;
    default: break;
    }
    ps_write32(guest_base(ctx), optval, v);
    return 0;
}

// int cellRudpGetContextStatus(int ctx_id, CellRudpContextStatus *status, size_t size): 1 to 48 bytes of: state,
// parentId, children (accepted ones), lostPackets, sentPackets, rcvdPackets, sentBytes, rcvdBytes,
// retransmissions, rtt (the smoothed round trip in microseconds).
s32 cellRudpGetContextStatus(PPUContext& ctx, s32 ctx_id, u32 status, u32 size)
{
    Locked l(ctx);
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0 || !status || size - 1 >= 0x30) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    s32 r = 0;
    Ctx* x = api_ctx(l, ctx_id, r);
    if (!x) return r;
    if (!usable(ctx, status, size, "cellRudpGetContextStatus")) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    const Conn& c = x->c;
    std::array<u8, 0x30> s{};
    ps_st32(s.data(), api_state(*x));
    ps_st32(s.data() + 4, x->valid && x->parent ? static_cast<u32>(x->parent->id) : 0xFFFFFFFF);
    ps_st32(s.data() + 8, static_cast<u32>(x->accepted.size()));
    ps_st32(s.data() + 0xC, c.lost);
    ps_st32(s.data() + 0x10, c.sent_packets);
    ps_st32(s.data() + 0x14, c.rcvd_packets);
    ps_st64(s.data() + 0x18, c.sent_bytes);
    ps_st64(s.data() + 0x20, c.rcvd_bytes);
    ps_st32(s.data() + 0x28, c.retransmissions);
    ps_st32(s.data() + 0x2C, static_cast<u32>(c.srtt));
    std::memcpy(guest_base(ctx) + status, s.data(), size);
    return 0;
}

// int cellRudpGetLocalInfo(int ctx_id, int *soc, sys_net_sockaddr *addr, socklen_t *addrlen, uint16_t *vport,
// uint8_t *muxmode): NOT_BOUND before Bind; the address is the socket's (getsockname), at most 16 bytes.
s32 cellRudpGetLocalInfo(PPUContext& ctx, s32 ctx_id, u32 soc, u32 addr, u32 addrlen, u32 vport, u32 muxmode)
{
    Locked l(ctx);
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    s32 r = 0;
    Ctx* x = api_ctx(l, ctx_id, r);
    if (!x) return r;
    if (x->soc == -1) return err(CELL_RUDP_ERROR_NOT_BOUND);
    u8* base = guest_base(ctx);
    if (soc && usable(ctx, soc, 4, "cellRudpGetLocalInfo: soc")) ps_write32(base, soc, static_cast<u32>(x->soc));
    if (addr && addrlen && usable(ctx, addrlen, 4, "cellRudpGetLocalInfo: addrlen")) {
        u32 n = ps_read32(base, addrlen);
        if (n > 16) {
            n = 16;
            ps_write32(base, addrlen, 16);
        }
        StackVar v(ctx, base, 0x30);
        ps_write32(base, v.addr() + 0x20, 16);
        libnet("getsockname", {sext(x->soc), v.addr(), v.addr() + 0x20});
        if (n && usable(ctx, addr, n, "cellRudpGetLocalInfo: addr")) std::memcpy(base + addr, base + v.addr(), n);
    }
    if (vport && usable(ctx, vport, 2, "cellRudpGetLocalInfo: vport")) ps_write16(base, vport, x->vport);
    if (muxmode && usable(ctx, muxmode, 1, "cellRudpGetLocalInfo: muxmode")) ps_write8(base, muxmode, x->mux);
    return 0;
}

// int cellRudpGetRemoteInfo(int ctx_id, sys_net_sockaddr *addr, socklen_t *addrlen, uint16_t *vport): connected
// contexts only (NOT_ACCEPTABLE); the peer's address, at most 34 bytes.
s32 cellRudpGetRemoteInfo(PPUContext& ctx, s32 ctx_id, u32 addr, u32 addrlen, u32 vport)
{
    Locked l(ctx);
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    s32 r = 0;
    Ctx* x = api_ctx(l, ctx_id, r);
    if (!x) return r;
    if ((x->c.state != kEstablished && x->c.state != kCloseWait) || (x->flags & kXListening))
        return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    u8* base = guest_base(ctx);
    if (vport && usable(ctx, vport, 2, "cellRudpGetRemoteInfo: vport"))
        ps_write16(base, vport, x->mux == 1 ? x->vport : x->peer.vport);
    if (addr && addrlen && usable(ctx, addrlen, 4, "cellRudpGetRemoteInfo: addrlen")) {
        const u32 n = std::min<u32>(ps_read32(base, addrlen), 0x22);
        ps_write32(base, addrlen, n);
        if (usable(ctx, addr, std::max<u32>(n, 2), "cellRudpGetRemoteInfo: addr")) {
            std::memcpy(base + addr, x->peer.raw.data(), n);
            ps_write8(base, addr + 1, x->peer.raw[1]);
        }
    }
    return 0;
}

// int cellRudpBind(int ctx_id, int soc, uint16_t vport, uint8_t muxmode): mux mode 1 or 2. librudp does not look at
// the socket (a socket that is not one only shows when sending fails).
s32 cellRudpBind(PPUContext& ctx, s32 ctx_id, s32 soc, u16 vport, u8 muxmode)
{
    Locked l(ctx);
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0 || soc < 0) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    if (static_cast<u8>(muxmode - 1) >= 2) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    return net_bind(l.lib, *l.lib.net, ctx_id, soc, vport, muxmode);
}

// int cellRudpListen(int ctx_id, int backlog): a context bound in mux mode 2; the event handler must be set.
s32 cellRudpListen(PPUContext& ctx, s32 ctx_id, s32 backlog)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0 || backlog < 1) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    Net& net = *lib.net;
    if (!net.handler) return err(CELL_RUDP_ERROR_NO_EVENT_HANDLER);
    s32 r = 0;
    Ctx* x = api_ctx(l, ctx_id, r);
    if (!x) return r;
    if (x->mux != 2 || x->c.state) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    if (x->flags & kXListening) return err(CELL_RUDP_ERROR_IN_PROGRESS);
    x->flags |= kXListening;
    for (Ctx::Pending& p : x->pending) mem_free(lib, p.mem);
    x->pending.clear();
    x->backlog = static_cast<u16>(std::min(backlog, 0x80));
    if (x->cookie_life) {
        u16 p[3];
        ctx_init_params(*x, p);
        x->secret = static_cast<u32>(p[0]) << 16 | p[1];
        if (!ctx_cookie_ring(lib, *x)) return err(CELL_RUDP_ERROR_MEMORY);
    }
    return net_prepare(lib, net, *x, nullptr, 0);
}

// Waits for a blocking context's connection (Initiate's, Activate's): its error when it failed, CANCELLED when it went.
s32 wait_connected(Lib& lib, PPUContext& ctx, Ctx& x, s32 r, bool activate)
{
    if (!(r == 0 || r == err(CELL_RUDP_ERROR_IN_PROGRESS)) || !x.blocking) return r;
    lv2::PpuThread& t = thread_of(ctx);
    x.waiters++;
    while (!(x.wake & kWakeCancel) && !(x.wake & (kWakeEstablished | kWakeClosed))) x.cond.wait(lib.lock, t, 0);
    const u8 w = x.wake;
    x.wake &= ~(kWakeEstablished | kWakeClosed);
    if (!--x.waiters && (x.wake & kWakeCancel)) x.cond.signal_all();
    if (activate) r = 0;
    if (w & kWakeClosed) {
        r = x.error;
        x.error = 0;
    } else if (w & kWakeCancel) {
        r = err(CELL_RUDP_ERROR_CANCELLED);
    }
    return r;
}

std::optional<std::array<u8, 0x22>> peer_arg(PPUContext& ctx, u32 to, u32 tolen)
{
    std::array<u8, 0x24> a{};
    if (tolen && !usable(ctx, to, tolen, "a peer's address")) return std::nullopt;
    if (tolen) std::memcpy(a.data(), guest_base(ctx) + to, tolen);
    std::array<u8, 0x22> out{};
    std::memcpy(out.data(), a.data(), 0x22);
    return out;
}

// int cellRudpInitiate(int ctx_id, const sys_net_sockaddr *to, socklen_t tolen, uint16_t vport)
s32 cellRudpInitiate(PPUContext& ctx, s32 ctx_id, u32 to, u32 tolen, u16 vport)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0 || !to || tolen > 0x22) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    Net& net = *lib.net;
    if (!net.handler) return err(CELL_RUDP_ERROR_NO_EVENT_HANDLER);
    const auto peer = peer_arg(ctx, to, tolen);
    if (!peer) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    s32 r = 0;
    Ctx* x = api_ctx(l, ctx_id, r);
    if (!x) return r;
    const u32 st = api_state(*x);
    if (st > 4) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    if (st == kEstablished) return err(CELL_RUDP_ERROR_ALREADY_ESTABLISHED);
    if (st <= 1) {
        if (const s32 p = net_prepare(lib, net, *x, &*peer, vport); p < 0) return p;
    }
    x->error = 0;
    return wait_connected(lib, ctx, *x, conn_initiate(lib, x->c), false);
}

// int cellRudpActivate(int ctx_id, const sys_net_sockaddr *to, socklen_t tolen): waits for that peer's SYN.
s32 cellRudpActivate(PPUContext& ctx, s32 ctx_id, u32 to, u32 tolen)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0 || !to || tolen > 0x22) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    Net& net = *lib.net;
    if (!net.handler) return err(CELL_RUDP_ERROR_NO_EVENT_HANDLER);
    const auto peer = peer_arg(ctx, to, tolen);
    if (!peer) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    s32 r = 0;
    Ctx* x = api_ctx(l, ctx_id, r);
    if (!x) return r;
    const u32 st = api_state(*x);
    if (st > 4) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    if (st == kEstablished) return err(CELL_RUDP_ERROR_ALREADY_ESTABLISHED);
    if (st <= 1) {
        if (const s32 p = net_prepare(lib, net, *x, &*peer, x->vport); p < 0) return p;
    }
    x->error = 0;
    return wait_connected(lib, ctx, *x, conn_activate(lib, x->c), true);
}

// int cellRudpAccept(int ctx_id, CellRudpContextEventHandler handler, void *arg, int *ctx_id_out,
// sys_net_sockaddr *addr, socklen_t *addrlen, uint16_t *vport): the oldest established child, which gets the
// handler; WOULDBLOCK when there is none and the listener does not block.
s32 cellRudpAccept(PPUContext& ctx, s32 ctx_id, u32 handler, u32 arg, u32 out_id, u32 addr, u32 addrlen, u32 vport)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0 || !out_id) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    if (!usable(ctx, out_id, 4, "cellRudpAccept")) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    s32 r = 0;
    Ctx* x = api_ctx(l, ctx_id, r);
    if (!x) return r;
    if (!(x->flags & kXListening)) return err(CELL_RUDP_ERROR_NOT_ACCEPTABLE);
    u8* base = guest_base(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    for (;;) {
        for (auto it = x->pending.begin(); it != x->pending.end(); ++it) {
            Ctx* child = it->ctx;
            if (child->c.state != kEstablished) continue;
            ps_write32(base, out_id, static_cast<u32>(child->id));
            mem_free(lib, it->mem);
            x->pending.erase(it);
            Ctx::ChildNode n;
            n.ctx = child;
            n.mem = mem_alloc(lib, 0x40);
            if (n.mem) x->accepted.emplace(child->peer, n);
            if (addr && addrlen && usable(ctx, addrlen, 4, "cellRudpAccept: addrlen")) {
                const u32 len = ps_read32(base, addrlen);
                std::array<u8, 0x24> raw{};
                std::memcpy(raw.data(), child->peer.raw.data(), 0x22);
                ps_st16(raw.data() + 0x22, child->peer.vport);
                if (usable(ctx, addr, std::max<u32>(len, 2), "cellRudpAccept: addr")) {
                    std::memcpy(base + addr, raw.data(), std::min<u32>(len, 0x24));
                    ps_write8(base, addr + 1, child->peer.raw[1]);
                }
                ps_write32(base, addrlen, child->peer.raw[0]);
            }
            if (vport && usable(ctx, vport, 2, "cellRudpAccept: vport")) ps_write16(base, vport, x->peer.vport);
            if (x->c.readable_size() > 0) ctx_post(*x, kQReadable);
            s32 e = 0;
            Ctx* got = ctx_find(*lib.net, static_cast<s32>(ps_read32(base, out_id)), false, &e);
            if (!got) return e;
            if (got->valid) {
                got->handler = handler;
                got->arg = arg;
            }
            return 0;
        }
        if (!x->blocking) return err(CELL_RUDP_ERROR_WOULDBLOCK);
        x->waiters++;
        while (!(x->wake & kWakeCancel) && !(x->wake & kWakeAcceptable)) x->cond.wait(lib.lock, t, 0);
        const u8 w = x->wake;
        x->waiters--;
        x->wake &= ~kWakeAcceptable;
        if (!x->waiters && (x->wake & kWakeCancel)) x->cond.signal_all();
        if (w & kWakeCancel) return err(CELL_RUDP_ERROR_CANCELLED);
    }
}

// int cellRudpTerminate(int ctx_id): a connection or a listener ends (and lingers until it is told it may go); an idle
// one goes at once.
s32 cellRudpTerminate(PPUContext& ctx, s32 ctx_id)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    Net& net = *lib.net;
    Ctx* x = ctx_find(net, ctx_id, true, nullptr);
    if (!x) return err(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
    if (x->valid && ((x->flags & kXListening) || (x->c.state >= kSynSent && x->c.state <= kCloseWait)))
        return ctx_terminate(lib, *x);
    ctx_delete(lib, net, ctx_id);
    return 0;
}

// ------------------------------------------------------------------------------------------------- the API: data

// Waits for a blocking call's event (`bits`) up to `timeout_ms` (0: none): 0, WOULDBLOCK when it ran out,
// CANCELLED when the context went.
s32 wait_event(Lib& lib, PPUContext& ctx, Ctx& x, u8 bits, u32 timeout_ms)
{
    lv2::PpuThread& t = thread_of(ctx);
    x.waiters++;
    s32 r = 0;
    if (!timeout_ms) {
        while (!(x.wake & kWakeCancel) && !(x.wake & bits)) x.cond.wait(lib.lock, t, 0);
    } else {
        while (!(x.wake & kWakeCancel) && !(x.wake & bits)) {
            if (!x.cond.wait(lib.lock, t, u64{timeout_ms} * 1000)) {
                if (!(x.wake & kWakeCancel) && !(x.wake & bits)) r = err(CELL_RUDP_ERROR_WOULDBLOCK);
                break;
            }
        }
    }
    const u8 w = x.wake;
    x.wake &= ~bits;
    if (!--x.waiters && (x.wake & kWakeCancel)) x.cond.signal_all();
    if (r < 0) return r;
    if (w & kWakeCancel) return err(CELL_RUDP_ERROR_CANCELLED);
    return 0;
}

// int cellRudpRead(int ctx_id, void *data, size_t len, uint8_t flags, CellRudpReadInfo *info): info->size (its first
// byte) at most 12; that many bytes of the information come back (its own first byte then 0).
s32 cellRudpRead(PPUContext& ctx, s32 ctx_id, u32 data, u32 len, u8 flags, u32 info)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0 || !data) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    u8* base = guest_base(ctx);
    u32 info_size = 0;
    if (info) {
        if (!usable(ctx, info, 1, "cellRudpRead: info")) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
        info_size = ps_read8(base, info);
        if (info_size >= 13) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    }
    s32 r = 0;
    Ctx* x = api_ctx(l, ctx_id, r);
    if (!x) return r;
    if (len && !usable(ctx, data, len, "cellRudpRead: data")) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    std::vector<u8> buf(len);
    std::array<u8, 12> ri{};
    Bytes out;
    out.p = buf.data();
    out.cap = len;
    r = conn_read(lib, x->c, out, info ? ri.data() : nullptr);
    if (r == err(CELL_RUDP_ERROR_WOULDBLOCK) && x->blocking && !(flags & kMsgDontwait)) {
        for (;;) {
            if (x->c.state != kEstablished && x->c.state != kCloseWait) break;
            const s32 w = wait_event(lib, ctx, *x, kWakeClosed | kWakeReadable, x->read_timeout);
            if (w < 0) {
                r = w == err(CELL_RUDP_ERROR_WOULDBLOCK) ? w : err(CELL_RUDP_ERROR_CANCELLED);
                break;
            }
            out = Bytes{buf.data(), 0, 0, len};
            r = conn_read(lib, x->c, out, info ? ri.data() : nullptr);
            if (r >= 0 || r != err(CELL_RUDP_ERROR_WOULDBLOCK)) break;
        }
    }
    if (r > 0) std::memcpy(base + data, buf.data(), static_cast<u32>(r));
    ri[0] = static_cast<u8>(info_size); // the size the game gave stays (librudp's)
    if (info && info_size && usable(ctx, info, info_size, "cellRudpRead: info")) std::memcpy(base + info, ri.data(), info_size);
    return r;
}

// int cellRudpWrite(int ctx_id, const void *data, size_t len, uint8_t flags): the bytes taken.
s32 cellRudpWrite(PPUContext& ctx, s32 ctx_id, u32 data, u32 len, u8 flags)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0 || !data || !len) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    s32 r = 0;
    Ctx* x = api_ctx(l, ctx_id, r);
    if (!x) return r;
    if (!usable(ctx, data, len, "cellRudpWrite: data")) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    std::vector<u8> buf(guest_base(ctx) + data, guest_base(ctx) + data + len);
    Bytes in{buf.data(), 0, len, len};
    r = conn_write(lib, x->c, in, flags);
    if (r == err(CELL_RUDP_ERROR_WOULDBLOCK) && x->blocking && !(flags & kMsgDontwait)) {
        while (x->c.state == kEstablished) {
            const s32 w = wait_event(lib, ctx, *x, kWakeClosed | kWakeWritable, x->write_timeout);
            if (w < 0) {
                r = w;
                break;
            }
            in = Bytes{buf.data(), 0, len, len};
            r = conn_write(lib, x->c, in, flags);
            if (r >= 0 || r != err(CELL_RUDP_ERROR_WOULDBLOCK)) break;
        }
    }
    return r;
}

// int cellRudpGetSizeReadable(int ctx_id)
s32 cellRudpGetSizeReadable(PPUContext& ctx, s32 ctx_id)
{
    Locked l(ctx);
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    s32 r = 0;
    Ctx* x = api_ctx(l, ctx_id, r);
    if (!x) return r;
    return x->c.readable_size();
}

// int cellRudpGetSizeWritable(int ctx_id)
s32 cellRudpGetSizeWritable(PPUContext& ctx, s32 ctx_id)
{
    Locked l(ctx);
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    s32 r = 0;
    Ctx* x = api_ctx(l, ctx_id, r);
    if (!x) return r;
    return x->c.writable();
}

// int cellRudpFlush(int ctx_id): what was written is marked; FLUSHED when the peer has all of it. A blocking context
// waits for it (up to FLUSH_TIMEOUT).
s32 cellRudpFlush(PPUContext& ctx, s32 ctx_id)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (ctx_id < 0) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    s32 r = 0;
    Ctx* x = api_ctx(l, ctx_id, r);
    if (!x) return r;
    r = conn_flush(x->c);
    if ((r == 0 || r == err(CELL_RUDP_ERROR_IN_PROGRESS)) && x->blocking) {
        const s32 w = wait_event(lib, ctx, *x, kWakeFlushed, x->flush_timeout);
        if (w < 0) r = w;
    }
    return r;
}

// ------------------------------------------------------------------------------------------------- the API: polling

// int cellRudpPollCreate(size_t size): the set's ID (from 0).
s32 cellRudpPollCreate(PPUContext& ctx, u32 size)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (!size) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    Net& net = *lib.net;
    Block obj = mem_alloc(lib, 0x60);
    if (!obj) return err(CELL_RUDP_ERROR_MEMORY);
    auto p = std::make_unique<PollSet>();
    p->mem_obj = obj;
    p->id = net.next_poll;
    net.next_poll = (net.next_poll + 1) & 0x7FFFFFFF;
    p->mem_ring = mem_alloc(lib, size * 4);
    if (p->mem_ring) p->ring.assign(size, 0);
    const s32 id = p->id;
    Block node = mem_alloc(lib, 0x20);
    if (node) net.polls.emplace(id, std::make_pair(std::move(p), node));
    return id;
}

// int cellRudpPollDestroy(int poll_id): its waits end (CANCELLED), its contexts leave it.
s32 cellRudpPollDestroy(PPUContext& ctx, s32 poll_id)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (poll_id < 0) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    Net& net = *lib.net;
    const auto it = net.polls.find(poll_id);
    if (it == net.polls.end()) return err(CELL_RUDP_ERROR_INVALID_POLL_ID);
    std::unique_ptr<PollSet> p = std::move(it->second.first);
    Block node = it->second.second;
    net.polls.erase(it);
    mem_free(lib, node);
    poll_destroy(lib, net, *p);
    mem_free(lib, p->mem_obj);
    return 0;
}

// int cellRudpPollControl(int poll_id, int op, int ctx_id, uint16_t events): 1 ADD, 2 MODIFY, 3 REMOVE.
s32 cellRudpPollControl(PPUContext& ctx, s32 poll_id, s32 op, s32 ctx_id, u16 events)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (poll_id < 0 || ctx_id < 0) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    Net& net = *lib.net;
    PollSet* p = poll_find(net, poll_id);
    if (!p) return err(CELL_RUDP_ERROR_INVALID_POLL_ID);
    if (op == 1) {
        s32 r = 0;
        Ctx* x = ctx_find(net, ctx_id, false, &r);
        if (!x) return r;
        if (x->poll > 0) return err(CELL_RUDP_ERROR_ALREADY_EXISTS);
        x->poll = p->id;
        const u16 ready = ctx_ready(*x);
        if (p->entries.count(ctx_id)) return err(CELL_RUDP_ERROR_ALREADY_EXISTS);
        const u32 n = static_cast<u32>(p->entries.size());
        if (p->ring.size() == n) {
            bool grown = false;
            if (n < 0x10000) {
                const u32 cap = n * 2;
                const u32 count = p->count();
                if (cap >= count) {
                    Block mem = mem_alloc(lib, cap * 4);
                    if (mem) {
                        std::vector<s32> ring(cap, 0);
                        for (u32 i = 0; i < count; i++) ring[i] = p->ring[(p->head + i) % p->ring.size()];
                        mem_free(lib, p->mem_ring);
                        p->mem_ring = mem;
                        p->ring = std::move(ring);
                        p->head = 0;
                        p->tail = count % std::max<u32>(cap, 1);
                        p->full = count == cap && cap;
                        grown = true;
                    }
                }
            }
            if (!grown) {
                x->poll = -1;
                return err(CELL_RUDP_ERROR_TOO_MANY_CONTEXTS);
            }
        }
        PollSet::Entry e;
        e.req = events;
        e.ret = static_cast<u16>(events & ready);
        e.mem = mem_alloc(lib, 0x20);
        if (e.mem) p->entries.emplace(ctx_id, e);
        if ((events & ready) && p->push(ctx_id)) {
            auto it = p->entries.find(ctx_id);
            if (it == p->entries.end()) {
                PollSet::Entry z;
                z.mem = mem_alloc(lib, 0x20);
                if (z.mem) it = p->entries.emplace(ctx_id, z);
            }
            if (it != p->entries.end()) it->second.ret |= 0x8000;
            p->cond.signal();
        }
        return 0;
    }
    if (op == 2) {
        Ctx* x = ctx_find(net, ctx_id, true, nullptr);
        const u16 ready = x ? ctx_ready(*x) : kPollError;
        const auto it = p->entries.find(ctx_id);
        if (it == p->entries.end()) return err(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
        PollSet::Entry& e = it->second;
        e.req = events;
        const u16 v = static_cast<u16>(((events | 0x8000) & e.ret) | (ready & events));
        e.ret = v;
        if (!(v & 0x8000) && (v & 0x7FFF) && p->push(ctx_id)) {
            e.ret = static_cast<u16>(v | 0x8000);
            p->cond.signal();
        }
        return 0;
    }
    if (op == 3) {
        if (Ctx* x = ctx_find(net, ctx_id, true, nullptr)) x->poll = -1;
        const auto it = p->entries.find(ctx_id);
        if (it == p->entries.end()) return err(CELL_RUDP_ERROR_INVALID_CONTEXT_ID);
        if (!(it->second.ret & 0x8000) && p->push(ctx_id)) p->cond.signal_all();
        mem_free(lib, it->second.mem);
        p->entries.erase(it);
        return 0;
    }
    return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
}

// int cellRudpPollWait(int poll_id, CellRudpPollEvent *events, size_t event_len, uint64_t timeout): microseconds,
// all ones for none; the number of events written.
s32 cellRudpPollWait(PPUContext& ctx, s32 poll_id, u32 events, u32 event_len, u64 timeout)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (poll_id < 0 || !events || !event_len) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    Net& net = *lib.net;
    PollSet* p = poll_find(net, poll_id);
    if (!p) return err(CELL_RUDP_ERROR_INVALID_POLL_ID);
    if (!usable(ctx, events, std::min<u32>(event_len, 0x10000) * 8, "cellRudpPollWait")) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = guest_base(ctx);
    const u64 start = now_us();
    // What the last wait handed out is looked at again: an error leaves the set, what is still ready queues again.
    for (u32 i = 0; i < p->carried; i++) {
        s32 id = -1;
        if (!p->ring.empty() && (p->tail != p->head || p->full)) {
            p->full = false;
            id = p->ring[p->head];
            if (++p->head == p->ring.size()) p->head = 0;
        }
        if (id < 0) continue;
        const auto it = p->entries.find(id);
        if (it == p->entries.end()) continue;
        PollSet::Entry& e = it->second;
        if (e.ret & kPollError) {
            mem_free(lib, e.mem);
            p->entries.erase(it);
            if (Ctx* x = ctx_find(net, id, true, nullptr)) x->poll = -1;
            continue;
        }
        Ctx* x = ctx_find(net, id, true, nullptr);
        const u16 ready = x ? ctx_ready(*x) : kPollError;
        const u16 v = static_cast<u16>((e.req & ready) | e.ret);
        e.ret = v;
        if (!(v & 0x7FFF) || !p->push(id)) e.ret = static_cast<u16>(v & 0x7FFF);
    }
    bool timed_out = false;
    u32 count = 0;
    for (;;) {
        count = p->count();
        if (count) break;
        if (timed_out || p->cancelling || p->cancel) break;
        u64 left = ~u64{0};
        if (timeout != ~u64{0}) {
            const u64 now = now_us();
            left = now < start + timeout ? start + timeout - now : 0;
        }
        p->waiters++;
        const bool woken = p->cond.wait(lib.lock, t, left ? left : 1);
        if (!--p->waiters && p->cancelling > 0) p->cond.signal_all();
        timed_out = !woken;
        if (!poll_find(net, poll_id)) return err(CELL_RUDP_ERROR_CANCELLED);
    }
    u32 written = 0, looked = 0;
    if (count && event_len) {
        for (u32 i = 0; i < count && written < event_len; i++) {
            looked = i + 1;
            const s32 id = p->ring[(p->head + i) % p->ring.size()];
            const u32 at = events + 8 * written;
            const auto it = p->entries.find(id);
            if (it == p->entries.end()) {
                ps_write32(base, at, static_cast<u32>(id));
                ps_write16(base, at + 4, 0);
                ps_write16(base, at + 6, kPollError);
                written++;
            } else if (it->second.ret & 0x8000) {
                ps_write32(base, at, static_cast<u32>(id));
                ps_write16(base, at + 4, it->second.req);
                ps_write16(base, at + 6, it->second.ret & 0x7FFF);
                it->second.ret &= 0x8008;
                written++;
            }
        }
        p->carried = looked;
        if (written) {
            p->cancel = false;
            return static_cast<s32>(written);
        }
    } else {
        p->carried = 0;
    }
    const bool cancelled = p->cancelling || p->cancel;
    p->cancel = false;
    return cancelled ? err(CELL_RUDP_ERROR_CANCELLED) : 0;
}

// int cellRudpPollCancel(int poll_id): the set's waits end with CANCELLED (the next one when nobody waits).
s32 cellRudpPollCancel(PPUContext& ctx, s32 poll_id)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (poll_id < 0) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    PollSet* p = poll_find(*lib.net, poll_id);
    if (!p) return err(CELL_RUDP_ERROR_INVALID_POLL_ID);
    poll_cancel(lib, *p, true, false);
    return 0;
}

// ------------------------------------------------------------------------------------------------- the API: I/O

// int cellRudpNetReceived(int soc, const uint8_t *data, size_t datalen, const sys_net_sockaddr *addr, socklen_t
// addrlen): a datagram the game received (without the internal I/O thread; THREAD_IN_USE with it).
s32 cellRudpNetReceived(PPUContext& ctx, s32 soc, u32 data, u32 datalen, u32 addr, u32 addrlen)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    if (soc < 0) return err(CELL_RUDP_ERROR_INVALID_SOCKET);
    if (!data) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    if (!datalen) return 0;
    if (!addr || addrlen >= 0x23) return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    if (!usable(ctx, data, datalen, "cellRudpNetReceived: data") || !usable(ctx, addr, addrlen, "cellRudpNetReceived: addr"))
        return err(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    std::array<u8, 0x22> from{};
    std::memcpy(from.data(), guest_base(ctx) + addr, std::min<u32>(addrlen, 0x22));
    std::vector<u8> bytes(guest_base(ctx) + data, guest_base(ctx) + data + datalen);
    return net_receive(lib, *lib.net, soc, bytes.data(), datalen, from, true);
}

// int cellRudpProcessEvents(uint64_t timeout): the scheduler on the calling thread (THREAD_IN_USE with the internal
// I/O thread): the timers and events due, then up to `timeout` microseconds for more.
s32 cellRudpProcessEvents(PPUContext& ctx, u64 timeout)
{
    Locked l(ctx);
    Lib& lib = l.lib;
    if (!l.ready()) return err(CELL_RUDP_ERROR_NOT_INITIALIZED);
    Net& net = *lib.net;
    if (net.io_enabled) return err(CELL_RUDP_ERROR_THREAD_IN_USE);
    return sched_run(lib, net, timeout);
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
    // No longer firmware-first (D-916 part 5): it matches librudp call for call.
}

} // namespace pspocket::hle
