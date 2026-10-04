// cellAudio (libaudio): audio ports, notify event queues and the audio thread.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellAudio.cpp/.h (upstream 431b169; not the iOS fork's
// g_cellaudio_follow_game pacing), Emu/Cell/lv2/sys_event.h and sys_sync.h (lv2_event_port IDs, the event source).
//
// A game opens up to 8 ports (4 for SDK 3.5 and older). A port is a ring of nBlock blocks in guest memory. Each block
// holds 256 samples of nChannel big-endian floats, and the game fills the blocks ahead of the port's read index. The
// audio thread plays one block of every started port per period (256 samples at 48 kHz, 5.33 ms). It mixes the block
// at the read index, clears it, and advances the read index (a u64 in guest memory that the game reads). Then it sends
// CELL_AUDIO_EVENT_MIX to every event queue registered with cellAudioSetNotifyEventQueue.
//
// Guest memory, in the main area (HleProcess::main_memory):
//   index area (64 KB, from cellAudioInit to cellAudioQuit): a port's read index, a be u64, at area + 16 * its server
//     index. Server indices 1..8 are handed out with the port numbers (RPCS3 free_ports, free_indices); slot 0 stays
//     unused.
//   port buffer (from cellAudioPortOpen to cellAudioPortClose): nBlock blocks of max(nChannel, 1) * 256 floats. The
//     allocation is the size rounded up to 64 KB (at least 64 KB), 64 KB aligned, like RPCS3's shared memory.
//   CellAudioPortParam {be u64 nChannel, nBlock, attr; be f32 level}.
//   CellAudioPortConfig {be u32 readIndexAddr, status; be u64 nChannel, nBlock; be u32 portSize, portAddr}.
// Besides clearing played blocks, the thread writes only the read indices. RPCS3 keeps each port's block counter and
// timestamp on the host (for cellAudioGetPortBlockTag and cellAudioGetPortTimestamp, not imported yet). It writes
// -0.0f tags into the blocks only in its buffering mode, which needs an audio backend; its null renderer turns it off.
//
// The audio thread is a host thread owned by the module, like RPCS3's cell_audio_thread. It is not an lv2 PPU thread
// like the GCM interrupt thread, because it runs no guest code. It touches only guest memory (under the module's
// lock, and only for open ports) and lv2 event queues it holds by shared_ptr (EventQueue::send needs no lv2 thread,
// as with lv2's timer service). So it needs no TLS, guest stack or internal function, the guest can't see it, and
// the kernel never waits for it. cellAudioInit starts it and cellAudioQuit stops and joins it. It stops by itself
// once the guest exits the process (Kernel::exited). The module state joins it when the HleProcess ends, which
// pspocket-boot and the tests do after Kernel::shutdown and before the kernel goes.
//
// Differences from RPCS3:
// - Memory: RPCS3 431b169 maps lv2 shared memory into sysPrxForUser's 256 MB shared memory area (key
//   SYS_MMAPPER_MIO_SHM_KEY for the index area, one object per port). Here these are main-area allocations, as
//   RPCS3's earlier vm::main variables were (g_audio_buffer, g_audio_indices). There are no lv2 memory objects, so
//   cellAudioInit's CELL_AUDIO_ERROR_TRANS_EVENT for an index area that is already mapped can't happen. A full main
//   area gives CELL_ENOMEM (init) or CELL_AUDIO_ERROR_SHAREDMEMORY (open).
// - Thread lifetime: RPCS3's thread runs for the whole emulation and keeps notifying registered queues after
//   cellAudioQuit. Here it runs from cellAudioInit to cellAudioQuit. Registered queues stay registered (RPCS3: "handled
//   independently") and get events again after the next cellAudioInit.
// - Timing: RPCS3 without buffering schedules period k at start + k * 5333 us and runs late periods back to back.
//   Here the schedule is exact (256 / 48000 s). A thread more than 100 ms late skips the missed periods instead of
//   running them in a burst, as the GCM vblank thread does.
// - Output: there is no audio backend. RPCS3 mixes every period into its ring buffer. Here the mix (RPCS3's
//   mix<STEREO, ...>) runs only while a host sink is attached (pspocket/hle/audio.hpp).
// - Queues by key: RPCS3 finds any event queue by its IPC key (lv2_event_queue::find). lv2's public interface finds
//   queues by ID only, so cellAudioSetNotifyEventQueue finds the queues cellAudioCreateNotifyEventQueue made. A keyed
//   queue the game created itself gives CELL_AUDIO_ERROR_TRANS_EVENT and an error log, until lv2 can look up a key.
// - RPCS3's short sleeps for the VSH (200 us in open, 30 us in start, 20 us in set) are left out, as cellGame's are
//   (D-044). So is its wait for libmixer's _mxr000 queue: there is no libmixer here.
// - Pointers are checked before use (D-042): a bad one gives CELL_AUDIO_ERROR_PARAM and the call site in the log.
//   cellAudioAddData on a port closed since it was opened gives CELL_AUDIO_ERROR_PORT_NOT_OPEN; RPCS3 writes into
//   the unmapped buffer.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/audio.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellAudio";

enum : u32 {
    CELL_AUDIO_ERROR_ALREADY_INIT = 0x80310701,
    CELL_AUDIO_ERROR_NOT_INIT = 0x80310703,
    CELL_AUDIO_ERROR_PARAM = 0x80310704,
    CELL_AUDIO_ERROR_PORT_FULL = 0x80310705,
    CELL_AUDIO_ERROR_PORT_ALREADY_RUN = 0x80310706,
    CELL_AUDIO_ERROR_PORT_NOT_OPEN = 0x80310707,
    CELL_AUDIO_ERROR_PORT_NOT_RUN = 0x80310708,
    CELL_AUDIO_ERROR_TRANS_EVENT = 0x80310709,
    CELL_AUDIO_ERROR_SHAREDMEMORY = 0x8031070b,
    CELL_AUDIO_ERROR_EVENT_QUEUE = 0x8031070d,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 CELL_AUDIO_BLOCK_SAMPLES = 256;
constexpr u32 CELL_AUDIO_MAX_PORT = 4, CELL_AUDIO_MAX_PORT_2 = 8;
constexpr u32 CELL_AUDIO_STATUS_CLOSE = 0x1010, CELL_AUDIO_STATUS_READY = 1, CELL_AUDIO_STATUS_RUN = 2;
constexpr u64 CELL_AUDIO_EVENT_MIX = 0;
constexpr u32 CELL_AUDIO_EVENTFLAG_BEFOREMIX = 0x80000000, CELL_AUDIO_EVENTFLAG_DECIMATE_2 = 0x08000000,
              CELL_AUDIO_EVENTFLAG_DECIMATE_4 = 0x10000000, CELL_AUDIO_EVENTFLAG_NOMIX = 0x40000000;
constexpr u64 CELL_AUDIO_PORTATTR_INITLEVEL = 0x1000;
// Port attributes RPCS3 accepts without emulating them: BGM (0x10), the secondary output (0x1), no route (0x100000)
// and the four personal outputs (0x01000000..0x08000000). Bits outside these and INITLEVEL are unknown.
constexpr u64 kPortAttrNotEmulated = 0x0F100011;
constexpr u64 kPortAttrKnown = kPortAttrNotEmulated | CELL_AUDIO_PORTATTR_INITLEVEL;

constexpr u32 AUDIO_PORT_COUNT = 8;
constexpr u32 MAX_AUDIO_EVENT_QUEUES = 64;
constexpr u32 kSdk35 = 0x35FFFF; // up to SDK 3.5: 4 ports, notify queues 2 events deep (RPCS3)
constexpr u32 kPortParamSize = 28;
constexpr u32 kPortConfigSize = 32;
constexpr u32 kIndexAreaSize = 0x10000; // RPCS3: the 64 KB MIO shared memory
constexpr u32 kIndexStride = 16;        // RPCS3: shared_address + server_index * 16
constexpr u32 kBufferGranule = 0x10000; // RPCS3 allocate_port: 64 KB shared memory pages
constexpr u64 kNotifyKeyBase = 0x80004D494F323221ull; // RPCS3 AudioCreateNotifyEventQueue: the first key tried
// RPCS3 builds the event source like an lv2 event port ID: lv2_event_port::id_base, lv2_obj's id_step and id_count.
constexpr u32 kEventPortIdBase = 0x0e000000, kEventPortIdStep = 0x100, kEventPortIdCount = 8192;
constexpr auto kMaxLateness = std::chrono::milliseconds(100);

const char* error_name(s32 r)
{
    switch (static_cast<u32>(r)) {
    case CELL_AUDIO_ERROR_ALREADY_INIT: return "CELL_AUDIO_ERROR_ALREADY_INIT";
    case CELL_AUDIO_ERROR_NOT_INIT: return "CELL_AUDIO_ERROR_NOT_INIT";
    case CELL_AUDIO_ERROR_PARAM: return "CELL_AUDIO_ERROR_PARAM";
    case CELL_AUDIO_ERROR_PORT_FULL: return "CELL_AUDIO_ERROR_PORT_FULL";
    case CELL_AUDIO_ERROR_PORT_ALREADY_RUN: return "CELL_AUDIO_ERROR_PORT_ALREADY_RUN";
    case CELL_AUDIO_ERROR_PORT_NOT_OPEN: return "CELL_AUDIO_ERROR_PORT_NOT_OPEN";
    case CELL_AUDIO_ERROR_PORT_NOT_RUN: return "CELL_AUDIO_ERROR_PORT_NOT_RUN";
    case CELL_AUDIO_ERROR_TRANS_EVENT: return "CELL_AUDIO_ERROR_TRANS_EVENT";
    case CELL_AUDIO_ERROR_SHAREDMEMORY: return "CELL_AUDIO_ERROR_SHAREDMEMORY";
    case CELL_AUDIO_ERROR_EVENT_QUEUE: return "CELL_AUDIO_ERROR_EVENT_QUEUE";
    }
    return lv2::error_name(r);
}

// Returns an error to the game, logged as a warning: these calls are rare, and the boot log should show what failed.
template <class... A>
s32 fail(s32 code, std::format_string<A...> fmt, A&&... args)
{
    PS_LOG_WARN(kChan, "{}: {}", std::format(fmt, std::forward<A>(args)...), error_name(code));
    return code;
}

enum class PortState : u8 { Closed, Opened, Started };

// RPCS3 audio_port, without the volume ramp of cellAudioSetPortLevel (not imported) and the buffering-mode tags.
struct Port {
    PortState state = PortState::Closed;
    u32 number = 0;
    u32 server_index = 0;
    bool mapped = false; // the buffer is allocated
    u32 addr = 0;        // the buffer
    u32 index = 0;       // the read index (be u64)
    u32 num_channels = 0;
    u32 num_blocks = 0;
    u64 attr = 0;
    u32 size = 0;           // of the nBlock blocks
    u64 cur_pos = 0;        // the block the next period plays
    u64 global_counter = 0; // the period counter at the port's last period (RPCS3: for block tags)
    u64 active_counter = 0; // periods played
    u64 timestamp = 0;      // guest system time (us) of the port's last period
    f32 level = 1.0f;

    u32 block_bytes() const { return std::max(num_channels, 1u) * CELL_AUDIO_BLOCK_SAMPLES * 4; }
    u32 block_addr(u64 block) const { return addr + static_cast<u32>(block) * block_bytes(); }
};

// RPCS3 cell_audio_thread::key_info: a queue that gets CELL_AUDIO_EVENT_MIX.
struct NotifyKey {
    u8 start_period = 0;
    u32 flags = 0;
    u64 source = 0; // the events' source: a made-up event port ID, as RPCS3
    std::shared_ptr<lv2::EventQueue> queue;
};

// One run of the audio thread, from cellAudioInit to cellAudioQuit.
struct Runner {
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false; // guarded by mutex
    std::thread thread;
};

using MixBlock = std::array<f32, kAudioBlockFrames * kAudioOutChannels>;

struct AudioState {
    explicit AudioState(HleProcess& p) : process(p), kernel(p.kernel()), base(p.kernel().base()) {}
    ~AudioState();
    AudioState(const AudioState&) = delete;
    AudioState& operator=(const AudioState&) = delete;

    HleProcess& process;
    lv2::Kernel& kernel;
    u8* const base;

    std::mutex mutex;              // RPCS3 cell_audio_thread::mutex: everything below until the sink
    std::atomic<bool> init{false}; // changed under `mutex`; some checks read it without (as RPCS3)
    u32 index_area = 0;            // RPCS3 shared_address
    u32 free_port_count = 0;
    std::array<u32, AUDIO_PORT_COUNT> free_ports{};
    std::array<u32, AUDIO_PORT_COUNT> free_indices{};
    std::array<Port, AUDIO_PORT_COUNT> ports{};
    std::vector<NotifyKey> keys;
    u32 key_count = 0;
    u8 event_period = 0;
    u64 counter = 0;         // RPCS3 m_counter: periods played since the first cellAudioInit
    u64 last_period_end = 0; // guest system time (us)
    // The queues cellAudioCreateNotifyEventQueue made, {key, ID}: what cellAudioSetNotifyEventQueue can find.
    std::vector<std::pair<u64, u32>> created_queues;
    std::unique_ptr<Runner> runner; // the running thread
    // Logs.
    std::array<bool, AUDIO_PORT_COUNT> config_logged{};
    bool add_data_logged = false;
    u32 polled_errors = 0;

    std::mutex sink_mutex; // held while the sink runs
    AudioSink sink;
    std::atomic<bool> has_sink{false};
};

AudioState& audio() { return HleProcess::current()->state<AudioState>(); }

// An error from a function games call every block or frame (cellAudioAddData, cellAudioGetPortConfig): logged like
// unimplemented calls (D-034), the first 8 times, then every 1024th. Called with s.mutex held.
template <class... A>
s32 fail_polled(AudioState& s, s32 code, std::format_string<A...> fmt, A&&... args)
{
    const u32 n = ++s.polled_errors;
    if (n <= 8 || n % 1024 == 0) {
        PS_LOG_WARN(kChan, "{}: {} (error #{})", std::format(fmt, std::forward<A>(args)...), error_name(code), n);
    }
    return code;
}

bool queue_alive(const std::shared_ptr<lv2::EventQueue>& q)
{
    if (!q) return false;
    std::lock_guard lock(q->lock);
    return !q->dead;
}

// RPCS3 lv2_event_queue::find(key), among the queues cellAudioCreateNotifyEventQueue made (see the top of the file).
std::shared_ptr<lv2::EventQueue> find_queue(AudioState& s, u64 key)
{
    for (const auto& [k, id] : s.created_queues) {
        if (k != key) continue;
        if (auto q = s.kernel.event_queue(id); q && q->key == key) return q; // null once destroyed
    }
    return nullptr;
}

void store_be64_release(u8* base, u32 addr, u64 value)
{
    __atomic_store_n(host_u64(base, addr), ps_bswap64(value), __ATOMIC_RELEASE);
}

// ------------------------------------------------------------------------------------------------- the audio thread

struct Notify {
    std::shared_ptr<lv2::EventQueue> queue;
    u64 source = 0;
    u64 data3 = 0;
};

// The start of period n of a run: exactly n * 256 / 48000 s (RPCS3 rounds the period to 5333 us).
std::chrono::nanoseconds period_offset(u64 n) { return std::chrono::nanoseconds(static_cast<s64>(n * 16'000'000 / 3)); }

// RPCS3 cell_audio_thread::mix<STEREO, downmix> for the stereo output cellSysutil reports, with RPCS3's default
// downmixer, CELL_AUDIO_OUT_DOWNMIXER_NONE (also what GTA V configures): an 8-channel port contributes its front left
// and right, a port without channels its one sample to both. RPCS3 takes the downmixer from cellAudioOutConfigure
// (TYPE_A: its Dolby stereo downmix of all channels); that is not followed yet. Master volume 1. With s.mutex held.
void mix_ports(AudioState& s, MixBlock& out)
{
    out.fill(0.0f);
    for (const Port& port : s.ports) {
        if (port.state != PortState::Started) continue;
        const u32 block = port.block_addr(port.cur_pos);
        const u32 stride = 4 * std::max(port.num_channels, 1u);
        for (u32 i = 0; i < kAudioBlockFrames; i++) {
            const u32 in = block + i * stride;
            const f32 left = ps_readf32(s.base, in) * port.level;
            const f32 right = port.num_channels ? ps_readf32(s.base, in + 4) * port.level : left;
            out[2 * i] += left;
            out[2 * i + 1] += right;
        }
    }
}

// RPCS3 cell_audio_thread::advance without buffering (so no tags): clear the block each started port just played,
// move the port to its next block and publish its read index, then pick the queues due for an event. With s.mutex
// held; the events go out once it is released, as in RPCS3.
void advance(AudioState& s, u64 now, std::vector<Notify>& notify)
{
    for (Port& port : s.ports) {
        if (port.state != PortState::Started) continue;
        std::memset(s.base + port.block_addr(port.cur_pos), 0, port.block_bytes());
        port.global_counter = s.counter;
        port.active_counter++;
        port.timestamp = now;
        port.cur_pos = (port.cur_pos + 1) % port.num_blocks;
        store_be64_release(s.base, port.index, port.cur_pos); // after the clear: the game may refill that block now
    }
    s.counter++;
    s.last_period_end = now;
    s.event_period++;
    for (const NotifyKey& key : s.keys) {
        if (key.flags & CELL_AUDIO_EVENTFLAG_NOMIX) continue;
        u32 periods = 1;
        if (key.flags & CELL_AUDIO_EVENTFLAG_DECIMATE_2) periods *= 2;
        if (key.flags & CELL_AUDIO_EVENTFLAG_DECIMATE_4) periods *= 4; // both: every 8th period
        if ((s.event_period ^ key.start_period) & (periods - 1)) continue;
        notify.push_back({key.queue, key.source, (key.flags & CELL_AUDIO_EVENTFLAG_BEFOREMIX) ? key.source : 0});
    }
}

// One period of the run `r` (RPCS3 cell_audio_thread::operator() without buffering: mix, enqueue, advance). Returns
// false when `r` is no longer the module's run (cellAudioQuit ran meanwhile).
bool run_period(AudioState& s, Runner& r, MixBlock& mix, std::vector<Notify>& notify)
{
    const bool mixing = s.has_sink.load(std::memory_order_acquire);
    {
        std::lock_guard lock(s.mutex);
        if (s.runner.get() != &r) return false;
        if (mixing) mix_ports(s, mix);
        advance(s, lv2::guest_system_time_us(), notify);
    }
    if (mixing) {
        std::lock_guard lock(s.sink_mutex);
        if (s.sink) s.sink(mix.data(), kAudioBlockFrames);
    }
    // A full queue drops the event (CELL_EBUSY) and a destroyed one refuses it; RPCS3 ignores both too.
    for (const Notify& n : notify) n.queue->send(lv2::Event{n.source, CELL_AUDIO_EVENT_MIX, 0, n.data3});
    notify.clear();
    return true;
}

// The thread's body: sleeps until each period's deadline, then plays the period.
void audio_thread(AudioState& s, Runner& r)
{
#if defined(__APPLE__)
    pthread_setname_np("cellAudio");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    using Clock = std::chrono::steady_clock;
    MixBlock mix{};
    std::vector<Notify> notify;
    auto start = Clock::now();
    for (u64 n = 0;; n++) {
        const auto deadline = start + period_offset(n);
        {
            std::unique_lock lock(r.mutex);
            if (r.cv.wait_until(lock, deadline, [&] { return r.stop; })) return;
        }
        if (s.kernel.exited()) {
            PS_LOG_INFO(kChan, "the process exited: the audio thread stops");
            return;
        }
        if (const auto now = Clock::now(); now - deadline > kMaxLateness) {
            start = now; // far behind (a debugger, a stalled host): skip the missed periods rather than burst
            n = 0;
        }
        if (!run_period(s, r, mix, notify)) return;
    }
}

// Starts a run of the thread (cellAudioInit, with s.mutex held, so its first period waits for the init to finish).
void start_thread(AudioState& s)
{
    auto r = std::make_unique<Runner>();
    Runner& run = *r;
    try {
        run.thread = std::thread([&s, &run] { audio_thread(s, run); });
    } catch (const std::system_error& e) {
        PS_FATAL(kChan, "cannot start the audio thread: {}", e.what());
    }
    s.runner = std::move(r);
}

// Ends a run: wakes the thread and joins it. Without s.mutex held (the thread takes it every period).
void stop_thread(std::unique_ptr<Runner> r)
{
    if (!r) return;
    {
        std::lock_guard lock(r->mutex);
        r->stop = true;
    }
    r->cv.notify_all();
    if (r->thread.joinable()) r->thread.join();
}

AudioState::~AudioState()
{
    std::unique_ptr<Runner> r;
    {
        std::lock_guard lock(mutex);
        r = std::move(runner);
    }
    stop_thread(std::move(r));
}

// ------------------------------------------------------------------------------------------------- ports

// RPCS3 cell_audio_thread::open_port: the port number and server index on top of the free lists.
Port* open_port(AudioState& s)
{
    if (!s.free_port_count) return nullptr;
    const u32 i = --s.free_port_count;
    Port& port = s.ports[s.free_ports[i]];
    port.server_index = s.free_indices[i];
    port.state = PortState::Opened;
    s.config_logged[port.number] = false;
    return &port;
}

// RPCS3 cell_audio_thread::close_port: the buffer goes, the port number and server index go back on the free lists.
void close_port(AudioState& s, Port& port)
{
    port.state = PortState::Closed;
    if (port.mapped) s.process.main_memory().free(port.addr);
    port.mapped = false;
    s.free_ports[s.free_port_count] = port.number;
    s.free_indices[s.free_port_count] = port.server_index;
    s.free_port_count++;
}

// RPCS3 cell_audio_thread::allocate_port: the port's buffer (zeroed) and its read index slot. The slot keeps whatever
// a previous port left in it until the port's first period, as in RPCS3.
s32 allocate_port(AudioState& s, Port& port)
{
    const u32 size = std::max(kBufferGranule, (port.size + kBufferGranule - 1) & ~(kBufferGranule - 1));
    const u32 addr = s.process.main_memory().alloc(size, kBufferGranule);
    if (!addr) {
        close_port(s, port);
        return fail(error(CELL_AUDIO_ERROR_SHAREDMEMORY), "cellAudioPortOpen: no main-area memory for {:#x} bytes",
            size);
    }
    port.addr = addr;
    port.mapped = true;
    port.index = s.index_area + port.server_index * kIndexStride;
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- functions

// cellAudioInit(): RPCS3's takes no arguments (GTA V leaves unrelated values in r3..r10).
s32 cellAudioInit(PPUContext&)
{
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (s.init) return fail(error(CELL_AUDIO_ERROR_ALREADY_INIT), "cellAudioInit()");
    const u32 area = s.process.main_memory().alloc(kIndexAreaSize);
    if (!area) return fail(lv2::err(lv2::CELL_ENOMEM), "cellAudioInit(): no main-area memory for the read indices");
    s.index_area = area;
    const u32 port_count = s.kernel.config().sdk_version <= kSdk35 ? CELL_AUDIO_MAX_PORT : CELL_AUDIO_MAX_PORT_2;
    s.free_port_count = port_count;
    for (u32 i = 0; i < AUDIO_PORT_COUNT; i++) {
        Port& port = s.ports[i];
        port.state = PortState::Closed;
        port.mapped = false;
        port.number = i;
        port.index = area + port.server_index * kIndexStride; // closed ports keep their buffer fields and slot
        if (i < port_count) {
            s.free_ports[i] = port_count - i - 1;
            s.free_indices[i] = port_count - i;
        }
    }
    s.last_period_end = lv2::guest_system_time_us();
    s.init = true;
    start_thread(s);
    PS_LOG_INFO(kChan, "cellAudioInit(): {} ports, read indices at {:#010x}; a period every 5.33 ms", port_count,
        area);
    return lv2::CELL_OK;
}

s32 cellAudioQuit(PPUContext&)
{
    AudioState& s = audio();
    std::unique_ptr<Runner> run;
    {
        std::lock_guard lock(s.mutex);
        if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioQuit()");
        for (Port& port : s.ports)
            if (port.state != PortState::Closed) close_port(s, port);
        s.process.main_memory().free(s.index_area);
        s.index_area = 0;
        s.init = false; // registered notify queues stay (RPCS3: "They are handled independently.")
        run = std::move(s.runner);
    }
    stop_thread(std::move(run));
    PS_LOG_INFO(kChan, "cellAudioQuit(): ports closed, the audio thread stopped");
    return lv2::CELL_OK;
}

// cellAudioPortOpen(CellAudioPortParam*, u32* portNum), with RPCS3's order of checks: init, pointers, channels,
// blocks, then a free port.
s32 cellAudioPortOpen(PPUContext& ctx, u32 param, u32 port_num)
{
    AudioState& s = audio();
    lv2::PpuThread& t = thread_of(ctx);
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioPortOpen(param {:#x})", param);
    if (!param || !port_num) {
        return fail(error(CELL_AUDIO_ERROR_PARAM), "cellAudioPortOpen(param {:#x}, portNum {:#x})", param, port_num);
    }
    if (!guest_readable(t, param, kPortParamSize) || !guest_readable(t, port_num, 4)) {
        log_call_site(ctx, s.base, "cellAudioPortOpen: CellAudioPortParam or portNum is not in guest memory");
        return error(CELL_AUDIO_ERROR_PARAM);
    }
    const u64 num_channels = ps_read64(s.base, param);
    const u64 num_blocks = ps_read64(s.base, param + 8);
    const u64 attr = ps_read64(s.base, param + 16);
    if (num_channels != 2 && num_channels != 8 && num_channels != 0) {
        return fail(error(CELL_AUDIO_ERROR_PARAM), "cellAudioPortOpen(param {:#x}): {} channels", param, num_channels);
    }
    if (num_blocks != 2 && num_blocks != 4 && num_blocks != 8 && num_blocks != 16 && num_blocks != 32) {
        return fail(error(CELL_AUDIO_ERROR_PARAM), "cellAudioPortOpen(param {:#x}): {} blocks", param, num_blocks);
    }
    if (attr & kPortAttrNotEmulated) {
        PS_LOG_WARN(kChan, "cellAudioPortOpen: attributes {:#x} (BGM, secondary or personal output, no route) are not "
                           "emulated (as RPCS3)",
            attr & kPortAttrNotEmulated);
    }
    if (attr & ~kPortAttrKnown) {
        PS_LOG_WARN(kChan, "cellAudioPortOpen: unknown attributes {:#x}", attr & ~kPortAttrKnown);
    }

    std::lock_guard lock(s.mutex);
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioPortOpen(param {:#x})", param);
    Port* port = open_port(s);
    if (!port) return fail(error(CELL_AUDIO_ERROR_PORT_FULL), "cellAudioPortOpen(param {:#x})", param);
    port->num_channels = static_cast<u32>(num_channels);
    port->num_blocks = static_cast<u32>(num_blocks);
    port->attr = attr;
    port->size = static_cast<u32>(std::max<u64>(1, num_channels) * num_blocks * CELL_AUDIO_BLOCK_SAMPLES * 4);
    port->cur_pos = 0;
    port->global_counter = s.counter;
    port->active_counter = 0;
    port->timestamp = s.last_period_end;
    port->level = (attr & CELL_AUDIO_PORTATTR_INITLEVEL) ? ps_readf32(s.base, param + 24) : 1.0f;
    if (const s32 r = allocate_port(s, *port); r != lv2::CELL_OK) return r;
    ps_write32(s.base, port_num, port->number);
    PS_LOG_INFO(kChan, "cellAudioPortOpen(param {:#x} {{nChannel {}, nBlock {}, attr {:#x}}}): port {}, buffer "
                       "{:#010x} ({:#x} bytes), read index at {:#010x}, level {}",
        param, num_channels, num_blocks, attr, port->number, port->addr, port->size, port->index, port->level);
    return lv2::CELL_OK;
}

s32 cellAudioGetPortConfig(PPUContext& ctx, u32 port_num, u32 config)
{
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail_polled(s, error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioGetPortConfig({})", port_num);
    if (!config || port_num >= AUDIO_PORT_COUNT) {
        return fail_polled(s, error(CELL_AUDIO_ERROR_PARAM), "cellAudioGetPortConfig({}, config {:#x})", port_num,
            config);
    }
    if (!guest_readable(thread_of(ctx), config, kPortConfigSize)) {
        log_call_site(ctx, s.base, "cellAudioGetPortConfig: CellAudioPortConfig is not in guest memory");
        return error(CELL_AUDIO_ERROR_PARAM);
    }
    const Port& port = s.ports[port_num];
    u32 status = CELL_AUDIO_STATUS_CLOSE;
    switch (port.state) {
    case PortState::Closed: status = CELL_AUDIO_STATUS_CLOSE; break;
    case PortState::Opened: status = CELL_AUDIO_STATUS_READY; break;
    case PortState::Started: status = CELL_AUDIO_STATUS_RUN; break;
    }
    ps_write32(s.base, config + 0x00, port.index); // readIndexAddr
    ps_write32(s.base, config + 0x04, status);
    ps_write64(s.base, config + 0x08, port.num_channels);
    ps_write64(s.base, config + 0x10, port.num_blocks);
    ps_write32(s.base, config + 0x18, port.size);
    ps_write32(s.base, config + 0x1C, port.addr);
    if (!s.config_logged[port_num]) { // once per port and opening: games may poll it
        s.config_logged[port_num] = true;
        PS_LOG_INFO(kChan, "cellAudioGetPortConfig({}): status {:#x}, read index at {:#010x}, buffer {:#010x}",
            port_num, status, port.index, port.addr);
    }
    return lv2::CELL_OK;
}

s32 cellAudioPortStart(PPUContext&, u32 port_num)
{
    AudioState& s = audio();
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioPortStart({})", port_num);
    if (port_num >= AUDIO_PORT_COUNT) return fail(error(CELL_AUDIO_ERROR_PARAM), "cellAudioPortStart({})", port_num);
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioPortStart({})", port_num);
    Port& port = s.ports[port_num];
    switch (port.state) {
    case PortState::Closed: return fail(error(CELL_AUDIO_ERROR_PORT_NOT_OPEN), "cellAudioPortStart({})", port_num);
    case PortState::Started: return fail(error(CELL_AUDIO_ERROR_PORT_ALREADY_RUN), "cellAudioPortStart({})", port_num);
    case PortState::Opened: break;
    }
    std::memset(s.base + port.addr, 0, port.size); // RPCS3: the whole ring starts silent
    port.state = PortState::Started;
    PS_LOG_INFO(kChan, "cellAudioPortStart({}): playing from block {}", port_num, port.cur_pos);
    return lv2::CELL_OK;
}

s32 cellAudioPortStop(PPUContext&, u32 port_num)
{
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioPortStop({})", port_num);
    if (port_num >= AUDIO_PORT_COUNT) return fail(error(CELL_AUDIO_ERROR_PARAM), "cellAudioPortStop({})", port_num);
    Port& port = s.ports[port_num];
    if (port.state != PortState::Started) {
        return fail(error(CELL_AUDIO_ERROR_PORT_NOT_RUN), "cellAudioPortStop({})", port_num); // closed or opened
    }
    port.state = PortState::Opened;
    PS_LOG_INFO(kChan, "cellAudioPortStop({}): stopped at read index {}", port_num, port.cur_pos);
    return lv2::CELL_OK;
}

s32 cellAudioPortClose(PPUContext&, u32 port_num)
{
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioPortClose({})", port_num);
    if (port_num >= AUDIO_PORT_COUNT) return fail(error(CELL_AUDIO_ERROR_PARAM), "cellAudioPortClose({})", port_num);
    Port& port = s.ports[port_num];
    if (port.state == PortState::Closed) {
        return fail(error(CELL_AUDIO_ERROR_PORT_NOT_OPEN), "cellAudioPortClose({})", port_num);
    }
    close_port(s, port);
    PS_LOG_INFO(kChan, "cellAudioPortClose({})", port_num);
    return lv2::CELL_OK;
}

// RPCS3 AudioCreateNotifyEventQueue for a PPU queue: an lv2 event queue (FIFO, 8 events deep, or 2 once cellAudioInit
// ran for SDK 3.5 and older) on the first free key from 0x80004D494F323221. It doesn't need cellAudioInit. RPCS3
// writes the key unchecked; here a bad key pointer is refused before the queue is made.
s32 cellAudioCreateNotifyEventQueue(PPUContext& ctx, u32 id, u32 key)
{
    AudioState& s = audio();
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, key, 8)) {
        log_call_site(ctx, s.base, "cellAudioCreateNotifyEventQueue: the key pointer is not in guest memory");
        return error(CELL_AUDIO_ERROR_PARAM);
    }
    StackVar attr(ctx, s.base, 16); // sys_event_queue_attribute_t {protocol, type, name[8] = 0}
    ps_write32(s.base, attr.addr(), lv2::SYS_SYNC_FIFO);
    ps_write32(s.base, attr.addr() + 4, lv2::SYS_PPU_QUEUE);
    for (u64 i = 0; i < MAX_AUDIO_EVENT_QUEUES; i++) {
        const u64 key_value = kNotifyKeyBase + i;
        const s32 depth = s.init && s.kernel.config().sdk_version <= kSdk35 ? 2 : 8;
        const s32 r = lv2::sys_event_queue_create(t, id, attr.addr(), key_value, depth);
        if (r == lv2::err(lv2::CELL_EEXIST)) continue; // the key is taken: the next one
        if (r != lv2::CELL_OK) return fail(r, "cellAudioCreateNotifyEventQueue(id {:#x}, key {:#x})", id, key);
        ps_write64(s.base, key, key_value);
        const u32 queue = ps_read32(s.base, id);
        {
            std::lock_guard lock(s.mutex);
            std::erase_if(s.created_queues,
                [&](const auto& e) { return e.first == key_value || !s.kernel.event_queue(e.second); });
            s.created_queues.emplace_back(key_value, queue);
        }
        PS_LOG_INFO(kChan, "cellAudioCreateNotifyEventQueue(id {:#x}, key {:#x}): queue {:#x}, key {:#x}, {} events",
            id, key, queue, key_value, depth);
        return lv2::CELL_OK;
    }
    return fail(error(CELL_AUDIO_ERROR_EVENT_QUEUE), "cellAudioCreateNotifyEventQueue(id {:#x}, key {:#x})", id, key);
}

// RPCS3 AudioSetNotifyEventQueue without flags: the queue with this key gets CELL_AUDIO_EVENT_MIX every period, with
// a source of its own.
s32 cellAudioSetNotifyEventQueue(PPUContext& ctx, u64 key)
{
    AudioState& s = audio();
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioSetNotifyEventQueue(key {:#x})", key);
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioSetNotifyEventQueue(key {:#x})", key);
    const auto q = find_queue(s, key);
    if (!q) {
        PS_LOG_ERROR(kChan, "cellAudioSetNotifyEventQueue(key {:#x}): no live queue from "
                            "cellAudioCreateNotifyEventQueue has this key (a keyed queue the game made itself is not "
                            "found: lv2 has no lookup by key yet): CELL_AUDIO_ERROR_TRANS_EVENT",
            key);
        return error(CELL_AUDIO_ERROR_TRANS_EVENT);
    }
    for (auto i = s.keys.begin(); i != s.keys.end();) {
        if (i->queue == q) {
            return fail(error(CELL_AUDIO_ERROR_TRANS_EVENT), "cellAudioSetNotifyEventQueue(key {:#x}): already set",
                key);
        }
        if (!queue_alive(i->queue)) {
            i = s.keys.erase(i); // a destroyed queue (RPCS3 cleans them up here)
        } else {
            ++i;
        }
    }
    const u64 pid = static_cast<u32>(lv2::sys_process_getpid(thread_of(ctx)));
    const u64 source = (pid << 32) + kEventPortIdBase + s.key_count++ * kEventPortIdStep;
    s.keys.push_back({s.event_period, 0, source, q});
    s.key_count %= kEventPortIdCount;
    PS_LOG_INFO(kChan, "cellAudioSetNotifyEventQueue(key {:#x}): queue {:#x} gets an event every period, source {:#x}",
        key, q->id, source);
    return lv2::CELL_OK;
}

// RPCS3 AudioRemoveNotifyEventQueue without flags.
s32 cellAudioRemoveNotifyEventQueue(PPUContext&, u64 key)
{
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioRemoveNotifyEventQueue(key {:#x})", key);
    for (auto i = s.keys.begin(); i != s.keys.end(); ++i) {
        if (!queue_alive(i->queue) || i->queue->key != key) continue;
        if (i->flags != 0) break; // registered with other flags (cellAudioSetNotifyEventQueueEx)
        const u32 queue = i->queue->id;
        s.keys.erase(i);
        PS_LOG_INFO(kChan, "cellAudioRemoveNotifyEventQueue(key {:#x}): queue {:#x} gets no more events", key, queue);
        return lv2::CELL_OK;
    }
    return fail(error(CELL_AUDIO_ERROR_TRANS_EVENT), "cellAudioRemoveNotifyEventQueue(key {:#x})", key);
}

// cellAudioAddData(portNum, float* src, samples, float volume): adds src * volume to the block at the port's read
// index, every channel. volume is a float argument, so it arrives in f1 (the typed binding passes integer registers
// only; RPCS3's binder reads ppu.fpr[1] for the first float argument).
s32 cellAudioAddData(PPUContext& ctx, u32 port_num, u32 src, u32 samples)
{
    f32 volume = static_cast<f32>(ctx.fpr[1]);
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail_polled(s, error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioAddData({})", port_num);
    if (port_num >= AUDIO_PORT_COUNT || !src || (src & 3) || samples != CELL_AUDIO_BLOCK_SAMPLES) {
        return fail_polled(s, error(CELL_AUDIO_ERROR_PARAM), "cellAudioAddData({}, src {:#x}, samples {})", port_num,
            src, samples);
    }
    const Port& port = s.ports[port_num];
    if (!port.num_channels) return lv2::CELL_OK; // RPCS3: nothing to add to (a port never opened, or without channels)
    if (!port.mapped) {
        return fail_polled(s, error(CELL_AUDIO_ERROR_PORT_NOT_OPEN),
            "cellAudioAddData({}): the port was closed and its buffer is gone (RPCS3 writes into it anyway)", port_num);
    }
    const u32 count = samples * port.num_channels;
    if (!guest_readable(thread_of(ctx), src, count * 4)) {
        log_call_site(ctx, s.base, "cellAudioAddData: the samples are not in guest memory");
        return error(CELL_AUDIO_ERROR_PARAM);
    }
    volume = std::isfinite(volume) ? std::clamp(volume, -16.0f, 16.0f) : 0.0f;
    const u32 block = static_cast<u16>(ps_read64(s.base, port.index)) & (port.num_blocks - 1);
    const u32 dst = port.block_addr(block);
    for (u32 i = 0; i < count; i++) {
        ps_writef32(s.base, dst + 4 * i, ps_readf32(s.base, dst + 4 * i) + ps_readf32(s.base, src + 4 * i) * volume);
    }
    if (!s.add_data_logged) {
        s.add_data_logged = true;
        PS_LOG_INFO(kChan, "first cellAudioAddData({}, src {:#x}, samples {}, volume {}): into block {}", port_num, src,
            samples, volume, block);
    }
    return lv2::CELL_OK;
}

} // namespace

void set_audio_sink(HleProcess& process, AudioSink sink)
{
    AudioState& s = process.state<AudioState>();
    std::lock_guard lock(s.sink_mutex);
    s.has_sink.store(static_cast<bool>(sink), std::memory_order_release);
    s.sink = std::move(sink);
}

void register_cellAudio(Registry& r)
{
    r.add("cellAudio", {
        PS_HLE_FUNC(0x0B168F92, cellAudioInit),
        PS_HLE_FUNC(0xCA5AC370, cellAudioQuit),
        PS_HLE_FUNC(0xCD7BC431, cellAudioPortOpen),
        PS_HLE_FUNC(0x4129FE2D, cellAudioPortClose),
        PS_HLE_FUNC(0x89BE28F2, cellAudioPortStart),
        PS_HLE_FUNC(0x5B1E2C73, cellAudioPortStop),
        PS_HLE_FUNC(0x74A66AF0, cellAudioGetPortConfig),
        PS_HLE_FUNC(0x04AF134E, cellAudioCreateNotifyEventQueue),
        PS_HLE_FUNC(0x377E0CD9, cellAudioSetNotifyEventQueue),
        PS_HLE_FUNC(0xFF3626FD, cellAudioRemoveNotifyEventQueue),
        PS_HLE_FUNC(0xDAB029AA, cellAudioAddData),
    });
}

} // namespace pspocket::hle
