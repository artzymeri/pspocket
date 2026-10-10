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
// Besides clearing played blocks, the thread writes only the read indices, and in buffering mode RPCS3's -0.0f tags into
// each block it clears (below). Each port's period counter and timestamp stay on the host, as in RPCS3, for
// cellAudioGetPortBlockTag and cellAudioGetPortTimestamp.
//
// Timing (D-074, pspocket/hle/audio.hpp). With a host sink that can say how much it holds (the speaker on the Mac and
// the iPhone), the thread runs RPCS3's buffering mode with its default settings (audio_buffering.hpp): each period
// lasts as long as keeps the host's buffer at its target (half to 1.2 periods), and before mixing it looks at the tags
// of the blocks it is about to play. A block whose tags the game hasn't overwritten yet is waited for, 1 ms at a time,
// if the game feeds that port (a tag moved in the last 340 ms), up to 2 periods after the previous one (4 when other
// ports are ready); a block the game is writing (a tag moved since the last look) gets 0.5 ms more. A game that runs late therefore plays whole
// blocks, one per period, instead of a silent block followed by a doubled one: cellAudioAddData adds to the block at
// the read index, so a late call lands on the next block. A game that leaves every block untouched for too long skips
// a period (nothing played). Without such a sink (pspocket-boot without --audio, the tests) the thread runs RPCS3
// without buffering: a fixed clock, and no tags.
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
//   Here the fixed clock is exact (256 / 48000 s). A thread more than 100 ms late skips the missed periods instead of
//   running them in a burst, as the GCM vblank thread does. Buffering mode leaves out RPCS3's alignment of a restart
//   to the global 5.33 ms clock, and replaces its time stretching (off by default) with the ps3-ios-engine port's
//   follow-the-game tempo when the sink has a stretcher (D-075).
// - Output: RPCS3 mixes every period into its ring buffer, and buffering depends on its backend. Here the mix
//   (RPCS3's mix<STEREO, ...>) runs only while a host sink is attached, and buffering only while the sink has a
//   buffer query (pspocket/hle/audio.hpp).
// - RPCS3's short sleeps for the VSH (200 us in open, 30 us in start, 20 us in set) are left out, as cellGame's are
//   (D-044). So is its wait for libmixer's _mxr000 queue: pspocket's libmixer (D-563) has none.
// - cellAudioSetPortLevel moves the level over 624 samples, as RPCS3's comment says; RPCS3's step has the wrong sign,
//   so there the level jumps at once.
// - Pointers are checked before use (D-042): a bad one gives CELL_AUDIO_ERROR_PARAM and the call site in the log.
//   cellAudioAddData on a port closed since it was opened gives CELL_AUDIO_ERROR_PORT_NOT_OPEN; RPCS3 writes into
//   the unmapped buffer.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
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

#include "audio_buffering.hpp"
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
    CELL_AUDIO_ERROR_TAG_NOT_FOUND = 0x8031070f,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 CELL_AUDIO_BLOCK_SAMPLES = 256;
constexpr u32 CELL_AUDIO_MAX_PORT = 4, CELL_AUDIO_MAX_PORT_2 = 8;
constexpr u32 CELL_AUDIO_STATUS_CLOSE = 0x1010, CELL_AUDIO_STATUS_READY = 1, CELL_AUDIO_STATUS_RUN = 2;
constexpr u64 CELL_AUDIO_EVENT_MIX = 0;
constexpr u32 CELL_AUDIO_EVENTFLAG_BEFOREMIX = 0x80000000, CELL_AUDIO_EVENTFLAG_DECIMATE_2 = 0x08000000,
              CELL_AUDIO_EVENTFLAG_DECIMATE_4 = 0x10000000, CELL_AUDIO_EVENTFLAG_NOMIX = 0x40000000;
constexpr u64 CELL_AUDIO_PORTATTR_INITLEVEL = 0x1000;
constexpr u32 CELL_AUDIO_CREATEEVENTFLAG_SPU = 1;
constexpr s32 CELL_AUDIO_PERSONAL_DEVICE_PRIMARY = 0x8000;
// RPCS3 cfg.audio_block_period: one period in whole microseconds, for cellAudioGetPortTimestamp.
constexpr u64 kBlockPeriodUs = CELL_AUDIO_BLOCK_SAMPLES * 1'000'000 / 48'000;
// RPCS3 cellAudioSetPortLevel: a new level is reached over 624 samples (13 ms).
constexpr f32 kLevelSteps = 624.0f;
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
    case CELL_AUDIO_ERROR_TAG_NOT_FOUND: return "CELL_AUDIO_ERROR_TAG_NOT_FOUND";
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

// RPCS3 audio_port, with pspocket's buffering-mode tags.
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
    f32 level_target = 1.0f; // cellAudioSetPortLevel: the level being moved to (RPCS3 level_set.value)
    f32 level_inc = 0.0f;    // per sample (RPCS3 level_set.inc); 0 when the level is reached
    audio_buffering::PortTags tags; // buffering mode: what the game has written of the next block (fresh at open)
    u32 adds = 0;                   // cellAudioAddData calls into the block at the read index (the sound line)

    u32 block_bytes() const { return std::max(num_channels, 1u) * CELL_AUDIO_BLOCK_SAMPLES * 4; }
    u32 block_addr(u64 block) const { return addr + static_cast<u32>(block) * block_bytes(); }
};

// RPCS3 cell_audio_thread::key_info: a queue that gets CELL_AUDIO_EVENT_MIX.
struct NotifyKey {
    u8 start_period = 0;
    u32 flags = 0;
    u64 source = 0; // the events' source: a made-up event port ID, as RPCS3
    std::shared_ptr<lv2::EventQueue> queue;
    u64 ack_timestamp = 0; // guest system time (us) of the last cellAudioSendAck for this source (kept, as RPCS3)
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
    std::unique_ptr<Runner> runner; // the running thread
    // Logs.
    std::array<bool, AUDIO_PORT_COUNT> config_logged{};
    bool add_data_logged = false;
    u32 polled_errors = 0;
    u32 add_data_calls = 0; // since the last sound line (buffering mode)
    u32 doubled = 0;        // blocks played with more than one cellAudioAddData in them, since the last sound line

    std::mutex sink_mutex; // held while the sink, its query or its stretcher runs
    AudioSink sink;
    AudioBufferQuery query;
    AudioStretch stretch;
    std::atomic<bool> has_sink{false};
    std::atomic<bool> buffered{false}; // a sink with a query: buffering mode
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

// RPCS3 lv2_event_queue::find(key).
std::shared_ptr<lv2::EventQueue> find_queue(AudioState& s, u64 key) { return s.kernel.event_queue_by_key(key); }

void store_be64_release(u8* base, u32 addr, u64 value)
{
    __atomic_store_n(host_u64(base, addr), ps_bswap64(value), __ATOMIC_RELEASE);
}

// A port's ring as the audio hardware's DMA reads and clears it, word by word: the game writes the ring while the audio
// thread mixes and clears it (ahead of the read position as it should, or over it when it runs late), so these are
// atomic and each word is one value or the other; relaxed, as the volatile loads and the memset they replace (D-587).
f32 ring_sample(u8* base, u32 addr)
{
    return std::bit_cast<f32>(ps_bswap32(__atomic_load_n(host_u32(base, addr), __ATOMIC_RELAXED)));
}
void clear_ring_block(u8* base, u32 addr, u32 bytes)
{
    u32* words = host_u32(base, addr);
    for (u32 i = 0; i < bytes / 4; i++) __atomic_store_n(words + i, 0u, __ATOMIC_RELAXED);
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
// (TYPE_A: its Dolby stereo downmix of all channels); that is not followed yet. Master volume 1. A level set with
// cellAudioSetPortLevel moves one step per sample (RPCS3 step_volume). With s.mutex held.
void mix_ports(AudioState& s, MixBlock& out)
{
    out.fill(0.0f);
    for (Port& port : s.ports) {
        if (port.state != PortState::Started) continue;
        const u32 block = port.block_addr(port.cur_pos);
        const u32 stride = 4 * std::max(port.num_channels, 1u);
        for (u32 i = 0; i < kAudioBlockFrames; i++) {
            if (port.level_inc != 0.0f) {
                port.level += port.level_inc;
                const bool up = port.level_inc > 0.0f;
                if ((up && port.level >= port.level_target) || (!up && port.level <= port.level_target)) {
                    port.level = port.level_target;
                    port.level_inc = 0.0f;
                }
            }
            const u32 in = block + i * stride;
            const f32 left = ring_sample(s.base, in) * port.level;
            const f32 right = port.num_channels ? ring_sample(s.base, in + 4) * port.level : left;
            out[2 * i] += left;
            out[2 * i + 1] += right;
        }
    }
}

// RPCS3 cell_audio_thread::advance: clear the block each started port just played (in buffering mode, tag it too),
// move the port to its next block and publish its read index, then pick the queues due for an event. With s.mutex
// held; the events go out once it is released, as in RPCS3.
void advance(AudioState& s, u64 now, std::vector<Notify>& notify, bool tag)
{
    for (Port& port : s.ports) {
        if (port.state != PortState::Started) continue;
        const u32 block = port.block_addr(port.cur_pos);
        clear_ring_block(s.base, block, port.block_bytes());
        if (tag && port.num_channels) audio_buffering::tag_block(s.base, block, port.num_channels, port.tags);
        s.doubled += port.adds > 1;
        port.adds = 0;
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

// What a period hands the sink: the mix, a silent block, or nothing.
enum class Output : u8 { None, Mix, Silence };

// One period of the run `r` (RPCS3 cell_audio_thread::operator(): mix, enqueue, advance). Returns false when `r` is no
// longer the module's run (cellAudioQuit ran meanwhile).
bool run_period(AudioState& s, Runner& r, MixBlock& mix, std::vector<Notify>& notify, Output out, bool tag)
{
    {
        std::lock_guard lock(s.mutex);
        if (s.runner.get() != &r) return false;
        if (out == Output::Mix) {
            mix_ports(s, mix);
        } else {
            // Nothing mixed: a level being moved gets there at once (RPCS3 finish_port_volume_stepping).
            for (Port& port : s.ports) {
                port.level = port.level_target;
                port.level_inc = 0.0f;
            }
        }
        advance(s, lv2::guest_system_time_us(), notify, tag);
    }
    if (out != Output::None) {
        if (out == Output::Silence) mix.fill(0.0f);
        std::lock_guard lock(s.sink_mutex);
        if (s.sink) s.sink(mix.data(), kAudioBlockFrames);
    }
    // A full queue drops the event (CELL_EBUSY) and a destroyed one refuses it; RPCS3 ignores both too.
    for (const Notify& n : notify) n.queue->send(lv2::Event{n.source, CELL_AUDIO_EVENT_MIX, 0, n.data3});
    notify.clear();
    return true;
}

// ---- buffering mode (D-074; the decisions are in audio_buffering.hpp)

constexpr u64 kSoundLogIntervalUs = 5'000'000;

// The buffering loop's state: RPCS3 cell_audio_thread's m_audio_should_restart, m_last_period_end and
// m_average_playtime, plus what the sound line in the log counts.
struct Buffering {
    bool restart = true;
    u64 last_period_end = 0; // host clock, us
    f32 average_us = 0.0f;   // what the host held after the periods, rolling average
    u64 desired_us = audio_buffering::desired_buffer_us(0);
    audio_buffering::Stretch stretch; // D-075
    // The sound line, every 5 s: periods by kind, those that waited for the game and the longest wait, and the range
    // the host's buffer stayed in.
    u64 log_since = 0;
    u32 mixed = 0, audible = 0, silent = 0, skipped = 0, late = 0;
    u64 wait_start = 0, longest_wait = 0;
    u32 low = ~0u, high = 0;
    u32 stretched = 0;     // periods played through the stretcher
    f32 slowest = 1.0f;    // the lowest tempo
};

u64 host_us()
{
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

u64 playtime_us(u32 frames) { return u64{frames} * 1'000'000 / kAudioSampleRate; }

// The sink's buffer level ({} once detached), under the sink's lock like the sink itself.
AudioBufferLevel buffer_level(AudioState& s)
{
    std::lock_guard lock(s.sink_mutex);
    return s.query ? s.query() : AudioBufferLevel{};
}

// How long what the host holds lasts at the speaker: longer while it plays stretched.
u64 speaker_us(u32 frames, const audio_buffering::Stretch& st)
{
    const u64 us = playtime_us(frames);
    return st.on ? static_cast<u64>(static_cast<f32>(us) / st.tempo) : us;
}

// Hands the stretcher its setting (nothing without a stretcher).
void apply_stretch(AudioState& s, const audio_buffering::Stretch& st)
{
    std::lock_guard lock(s.sink_mutex);
    if (s.stretch) s.stretch(st.on, st.tempo);
}

// Waits `us`; false when the run was told to stop meanwhile.
bool pause(Runner& r, u64 us)
{
    std::unique_lock lock(r.mutex);
    return !r.cv.wait_for(lock, std::chrono::microseconds(us), [&] { return r.stop; });
}

// RPCS3 count_port_buffer_tags over the started ports, at the block each plays next. With s.mutex held.
audio_buffering::TagCount count_tags(AudioState& s)
{
    audio_buffering::TagCount c;
    for (Port& port : s.ports) {
        if (port.state != PortState::Started) continue;
        c.active++;
        if (!port.num_channels) continue;
        const audio_buffering::Block block =
            audio_buffering::classify_block(s.base, port.block_addr(port.cur_pos), port.num_channels, port.tags);
        audio_buffering::count(c, block, port.tags.fed);
        if (port.tags.front_only && !port.tags.front_only_reported) {
            port.tags.front_only_reported = true;
            PS_LOG_INFO(kChan, "port {} carries front channel audio only ({} channels): its marks, not its tags, tell "
                               "whether the game wrote a block (as RPCS3)",
                port.number, port.num_channels);
        }
    }
    return c;
}

// One turn of RPCS3's loop in buffering mode: wait (for the period's end, or for the game), or play a period. False
// when the thread must end.
bool buffered_turn(AudioState& s, Runner& r, Buffering& b, MixBlock& mix, std::vector<Notify>& notify)
{
    namespace ab = audio_buffering;
    const u64 now = host_us();
    AudioBufferLevel level = buffer_level(s);
    b.desired_us = ab::desired_buffer_us(level.device_frames); // the device's callback length is known once it played
    if (b.restart) {
        // RPCS3's restart: the host's buffer starts with the target's worth of silence.
        b.restart = false;
        mix.fill(0.0f);
        {
            std::lock_guard lock(s.sink_mutex);
            for (u32 i = 0; s.sink && i < ab::restart_blocks(b.desired_us); i++) s.sink(mix.data(), kAudioBlockFrames);
        }
        b.stretch = {};
        apply_stretch(s, b.stretch);
        b.average_us = static_cast<f32>(playtime_us(buffer_level(s).queued));
        b.last_period_end = now;
        b.log_since = now;
        return true;
    }
    // Time stretching (D-075): the tempo follows the game's pace, trimmed toward the target fill.
    u64 enqueued_us = speaker_us(level.queued, b.stretch);
    if (b.stretch.update(static_cast<f32>(enqueued_us) / ab::adjusted_target_us(b.average_us, b.desired_us), now)) {
        apply_stretch(s, b.stretch);
        enqueued_us = speaker_us(level.queued, b.stretch);
    }
    ab::TagCount tags;
    {
        std::lock_guard lock(s.mutex);
        if (s.runner.get() != &r) return false;
        tags = count_tags(s);
    }
    const ab::Decision d = ab::decide(now - b.last_period_end, enqueued_us, b.average_us, b.desired_us, tags);
    Output out = Output::Mix;
    switch (d.step) {
    case ab::Step::WaitForGame:
    case ab::Step::WaitForWrites:
        if (!b.wait_start) b.wait_start = now;
        return pause(r, d.wait_us);
    case ab::Step::Wait: return pause(r, d.wait_us);
    case ab::Step::Silence: out = Output::Silence; break;
    case ab::Step::Skip: out = Output::None; break;
    case ab::Step::Mix: out = Output::Mix; break;
    }
    if (!run_period(s, r, mix, notify, out, true)) return false;
    b.stretch.period(now - b.last_period_end, b.wait_start != 0, out != Output::None);
    b.last_period_end = now;
    level = buffer_level(s);
    b.average_us = ab::kPeriodAverageAlpha * static_cast<f32>(speaker_us(level.queued, b.stretch)) +
                   (1.0f - ab::kPeriodAverageAlpha) * b.average_us;

    b.mixed += out == Output::Mix;
    b.audible += out == Output::Mix && std::any_of(mix.begin(), mix.end(), [](f32 v) { return v != 0.0f; });
    b.silent += out == Output::Silence;
    b.skipped += out == Output::None;
    if (b.wait_start) {
        b.late++;
        b.longest_wait = std::max(b.longest_wait, now - b.wait_start);
        b.wait_start = 0;
    }
    b.low = std::min(b.low, level.queued);
    b.high = std::max(b.high, level.queued);
    if (b.stretch.on) {
        b.stretched++;
        b.slowest = std::min(b.slowest, b.stretch.tempo);
    }
    if (now - b.log_since >= kSoundLogIntervalUs) {
        u32 added = 0, doubled = 0;
        {
            std::lock_guard lock(s.mutex);
            added = std::exchange(s.add_data_calls, 0);
            doubled = std::exchange(s.doubled, 0);
        }
        PS_LOG_INFO(kChan, "sound: {} periods in {:.1f} s ({} mixed, {} of them not silent; {} silent; {} skipped: "
                           "the game was too late); {} waited for the game, the longest {:.1f} ms; {} "
                           "cellAudioAddData, {} blocks with two or more; the host held {:.0f} to {:.0f} ms (target "
                           "{} ms); the game's pace {:.2f}, {} periods stretched, the slowest at {:.2f}",
            b.mixed + b.silent + b.skipped, static_cast<double>(now - b.log_since) / 1e6, b.mixed, b.audible,
            b.silent, b.skipped, b.late, static_cast<double>(b.longest_wait) / 1e3, added, doubled,
            static_cast<double>(playtime_us(b.low)) / 1e3, static_cast<double>(playtime_us(b.high)) / 1e3,
            b.desired_us / 1000, b.stretch.pace, b.stretched, b.slowest);
        b.log_since = now;
        b.mixed = b.audible = b.silent = b.skipped = b.late = 0;
        b.longest_wait = 0;
        b.low = ~0u;
        b.high = 0;
        b.stretched = 0;
        b.slowest = 1.0f;
    }
    return true;
}

// The thread's body: buffering mode while the sink has a buffer query, otherwise a fixed clock that sleeps until each
// period's deadline.
void audio_thread(AudioState& s, Runner& r)
{
#if defined(__APPLE__)
    pthread_setname_np("cellAudio");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    using Clock = std::chrono::steady_clock;
    MixBlock mix{};
    std::vector<Notify> notify;
    Buffering buffering;
    bool buffered = false;
    auto start = Clock::now();
    u64 n = 0; // fixed clock: the next period since `start`
    for (;;) {
        if (s.kernel.exited()) {
            PS_LOG_INFO(kChan, "the process exited: the audio thread stops");
            return;
        }
        if (const bool now_buffered = s.buffered.load(std::memory_order_acquire); now_buffered != buffered) {
            buffered = now_buffered; // a sink with a query came or went: start over in the other mode
            buffering = Buffering{};
            start = Clock::now();
            n = 0;
        }
        if (buffered) {
            if (!buffered_turn(s, r, buffering, mix, notify)) return;
            continue;
        }
        const auto deadline = start + period_offset(n);
        {
            std::unique_lock lock(r.mutex);
            if (r.cv.wait_until(lock, deadline, [&] { return r.stop; })) return;
        }
        if (s.kernel.exited() || s.buffered.load(std::memory_order_acquire)) continue; // the top of the loop sees to it
        if (const auto now = Clock::now(); now - deadline > kMaxLateness) {
            start = now; // far behind (a debugger, a stalled host): skip the missed periods rather than burst
            n = 0;
        }
        const Output out = s.has_sink.load(std::memory_order_acquire) ? Output::Mix : Output::None;
        if (!run_period(s, r, mix, notify, out, false)) return;
        n++;
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
    port.tags = {};
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
    port->level_target = port->level;
    port->level_inc = 0.0f;
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
    port.tags = {}; // and not fed until the game writes it (D-074)
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

// RPCS3 AudioCreateNotifyEventQueue: an lv2 event queue of the given type (FIFO, 8 events deep, or 2 once cellAudioInit
// ran for SDK 3.5 and older) on the first free key from 0x80004D494F323221. It doesn't need cellAudioInit. RPCS3
// writes the key unchecked; here a bad key pointer is refused before the queue is made.
s32 create_notify_queue(PPUContext& ctx, u32 id, u32 key, u32 queue_type)
{
    AudioState& s = audio();
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, key, 8)) {
        log_call_site(ctx, s.base, "cellAudioCreateNotifyEventQueue: the key pointer is not in guest memory");
        return error(CELL_AUDIO_ERROR_PARAM);
    }
    StackVar attr(ctx, s.base, 16); // sys_event_queue_attribute_t {protocol, type, name[8] = 0}
    ps_write32(s.base, attr.addr(), lv2::SYS_SYNC_FIFO);
    ps_write32(s.base, attr.addr() + 4, queue_type);
    for (u64 i = 0; i < MAX_AUDIO_EVENT_QUEUES; i++) {
        const u64 key_value = kNotifyKeyBase + i;
        const s32 depth = s.init && s.kernel.config().sdk_version <= kSdk35 ? 2 : 8;
        const s32 r = lv2::sys_event_queue_create(t, id, attr.addr(), key_value, depth);
        if (r == lv2::err(lv2::CELL_EEXIST)) continue; // the key is taken: the next one
        if (r != lv2::CELL_OK) return fail(r, "cellAudioCreateNotifyEventQueue(id {:#x}, key {:#x})", id, key);
        ps_write64(s.base, key, key_value);
        const u32 queue = ps_read32(s.base, id);
        PS_LOG_INFO(kChan, "cellAudioCreateNotifyEventQueue(id {:#x}, key {:#x}): queue {:#x}, key {:#x}, {} events",
            id, key, queue, key_value, depth);
        return lv2::CELL_OK;
    }
    return fail(error(CELL_AUDIO_ERROR_EVENT_QUEUE), "cellAudioCreateNotifyEventQueue(id {:#x}, key {:#x})", id, key);
}

s32 cellAudioCreateNotifyEventQueue(PPUContext& ctx, u32 id, u32 key)
{
    return create_notify_queue(ctx, id, key, lv2::SYS_PPU_QUEUE);
}

// cellAudioCreateNotifyEventQueueEx(id, key, iFlags): CELL_AUDIO_CREATEEVENTFLAG_SPU makes an SPU queue.
s32 cellAudioCreateNotifyEventQueueEx(PPUContext& ctx, u32 id, u32 key, u32 flags)
{
    if (flags & ~CELL_AUDIO_CREATEEVENTFLAG_SPU) {
        return fail(error(CELL_AUDIO_ERROR_PARAM), "cellAudioCreateNotifyEventQueueEx(flags {:#x})", flags);
    }
    return create_notify_queue(ctx, id, key,
        (flags & CELL_AUDIO_CREATEEVENTFLAG_SPU) ? lv2::SYS_SPU_QUEUE : lv2::SYS_PPU_QUEUE);
}

// RPCS3 AudioSetNotifyEventQueue: the queue with this key gets CELL_AUDIO_EVENT_MIX every period (or every 2nd, 4th or
// 8th with the decimate flags, none with NOMIX), with a source of its own.
s32 set_notify_queue(PPUContext& ctx, u64 key, u32 flags)
{
    AudioState& s = audio();
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioSetNotifyEventQueue(key {:#x})", key);
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioSetNotifyEventQueue(key {:#x})", key);
    const auto q = find_queue(s, key);
    if (!q) return fail(error(CELL_AUDIO_ERROR_TRANS_EVENT), "cellAudioSetNotifyEventQueue(key {:#x}): no queue", key);
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
    s.keys.push_back({s.event_period, flags, source, q});
    s.key_count %= kEventPortIdCount;
    PS_LOG_INFO(kChan, "cellAudioSetNotifyEventQueue(key {:#x}, flags {:#x}): queue {:#x} gets events, source {:#x}",
        key, flags, q->id, source);
    return lv2::CELL_OK;
}

s32 cellAudioSetNotifyEventQueue(PPUContext& ctx, u64 key) { return set_notify_queue(ctx, key, 0); }

// The Ex variants take the event flags in the top 5 bits; any other bit is CELL_AUDIO_ERROR_PARAM (RPCS3).
constexpr u32 kNotifyFlagsInvalid = ~0u >> 5;

s32 cellAudioSetNotifyEventQueueEx(PPUContext& ctx, u64 key, u32 flags)
{
    if (flags & kNotifyFlagsInvalid) {
        return fail(error(CELL_AUDIO_ERROR_PARAM), "cellAudioSetNotifyEventQueueEx(key {:#x}, flags {:#x})", key, flags);
    }
    return set_notify_queue(ctx, key, flags);
}

// RPCS3 AudioRemoveNotifyEventQueue: the queue must have been set with the same flags.
s32 remove_notify_queue(u64 key, u32 flags)
{
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioRemoveNotifyEventQueue(key {:#x})", key);
    for (auto i = s.keys.begin(); i != s.keys.end(); ++i) {
        if (!queue_alive(i->queue) || i->queue->key != key) continue;
        if (i->flags != flags) break; // registered with other flags
        const u32 queue = i->queue->id;
        s.keys.erase(i);
        PS_LOG_INFO(kChan, "cellAudioRemoveNotifyEventQueue(key {:#x}): queue {:#x} gets no more events", key, queue);
        return lv2::CELL_OK;
    }
    return fail(error(CELL_AUDIO_ERROR_TRANS_EVENT), "cellAudioRemoveNotifyEventQueue(key {:#x})", key);
}

s32 cellAudioRemoveNotifyEventQueue(PPUContext&, u64 key) { return remove_notify_queue(key, 0); }

s32 cellAudioRemoveNotifyEventQueueEx(PPUContext&, u64 key, u32 flags)
{
    if (flags & kNotifyFlagsInvalid) {
        return fail(error(CELL_AUDIO_ERROR_PARAM), "cellAudioRemoveNotifyEventQueueEx(key {:#x}, flags {:#x})", key,
            flags);
    }
    return remove_notify_queue(key, flags);
}

// cellAudioSendAck(data3): the game acknowledges an event; RPCS3 records when, for the key with that source.
s32 cellAudioSendAck(PPUContext&, u64 data3)
{
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioSendAck({:#x})", data3);
    for (NotifyKey& k : s.keys) {
        if (k.source != data3) continue;
        k.ack_timestamp = lv2::guest_system_time_us();
        break;
    }
    return lv2::CELL_OK;
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
    Port& port = s.ports[port_num];
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
    s.add_data_calls++;
    port.adds++;
    if (!s.add_data_logged) {
        s.add_data_logged = true;
        PS_LOG_INFO(kChan, "first cellAudioAddData({}, src {:#x}, samples {}, volume {}): into block {}", port_num, src,
            samples, volume, block);
    }
    return lv2::CELL_OK;
}

// The block at a port's read index, for the Add*Data functions (RPCS3 audio_port::get_vm_ptr), or an error: like
// cellAudioAddData, a port whose buffer is gone is CELL_AUDIO_ERROR_PORT_NOT_OPEN, where RPCS3 writes into it anyway.
// 0 with r == OK: the port has no channels (never opened), nothing to add to.
u32 add_target(AudioState& s, const Port& port, const char* fn, s32& r)
{
    r = lv2::CELL_OK;
    if (!port.num_channels) return 0;
    if (!port.mapped) {
        r = fail_polled(s, error(CELL_AUDIO_ERROR_PORT_NOT_OPEN), "{}({}): the port was closed", fn, port.number);
        return 0;
    }
    return port.block_addr(static_cast<u16>(ps_read64(s.base, port.index)) & (port.num_blocks - 1));
}

f32 clamp_volume(f32 v) { return std::isfinite(v) ? std::clamp(v, -16.0f, 16.0f) : 0.0f; }

// cellAudioAdd2chData(portNum, float* src, samples, float volume): stereo samples into a 2- or 8-channel port's front
// left and right (RPCS3). volume in f1.
s32 cellAudioAdd2chData(PPUContext& ctx, u32 port_num, u32 src, u32 samples)
{
    const f32 volume = clamp_volume(static_cast<f32>(ctx.fpr[1]));
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail_polled(s, error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioAdd2chData({})", port_num);
    if (port_num >= AUDIO_PORT_COUNT || !src || (src & 3) || samples != CELL_AUDIO_BLOCK_SAMPLES) {
        return fail_polled(s, error(CELL_AUDIO_ERROR_PARAM), "cellAudioAdd2chData({}, src {:#x}, samples {})",
            port_num, src, samples);
    }
    if (!guest_readable(thread_of(ctx), src, samples * 2 * 4)) {
        log_call_site(ctx, s.base, "cellAudioAdd2chData: the samples are not in guest memory");
        return error(CELL_AUDIO_ERROR_PARAM);
    }
    const Port& port = s.ports[port_num];
    if (port.num_channels != 2 && port.num_channels != 8) {
        PS_LOG_ERROR(kChan, "cellAudioAdd2chData({}): a port of {} channels", port_num, port.num_channels);
        return lv2::CELL_OK; // RPCS3
    }
    s32 r;
    const u32 dst = add_target(s, port, "cellAudioAdd2chData", r);
    if (!dst) return r;
    for (u32 i = 0; i < samples; i++) {
        for (u32 c = 0; c < 2; c++) {
            const u32 d = dst + 4 * (i * port.num_channels + c);
            ps_writef32(s.base, d, ps_readf32(s.base, d) + ps_readf32(s.base, src + 4 * (i * 2 + c)) * volume);
        }
    }
    return lv2::CELL_OK;
}

// cellAudioAdd6chData(portNum, float* src, float volume): a block of 5.1 samples (front left, centre, front right,
// surround left, surround right, LFE) into an 8-channel port's channels 0 to 5 in its order (L, R, C, LFE, rear L,
// rear R; RPCS3). volume in f1.
s32 cellAudioAdd6chData(PPUContext& ctx, u32 port_num, u32 src)
{
    const f32 volume = clamp_volume(static_cast<f32>(ctx.fpr[1]));
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail_polled(s, error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioAdd6chData({})", port_num);
    if (port_num >= AUDIO_PORT_COUNT || !src || (src & 3)) {
        return fail_polled(s, error(CELL_AUDIO_ERROR_PARAM), "cellAudioAdd6chData({}, src {:#x})", port_num, src);
    }
    if (!guest_readable(thread_of(ctx), src, CELL_AUDIO_BLOCK_SAMPLES * 6 * 4)) {
        log_call_site(ctx, s.base, "cellAudioAdd6chData: the samples are not in guest memory");
        return error(CELL_AUDIO_ERROR_PARAM);
    }
    const Port& port = s.ports[port_num];
    if (port.num_channels != 8) {
        PS_LOG_ERROR(kChan, "cellAudioAdd6chData({}): a port of {} channels", port_num, port.num_channels);
        return lv2::CELL_OK; // RPCS3
    }
    s32 r;
    const u32 dst = add_target(s, port, "cellAudioAdd6chData", r);
    if (!dst) return r;
    static constexpr u32 kFrom[6] = {0, 2, 1, 5, 3, 4}; // port channel c takes source channel kFrom[c]
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES; i++) {
        for (u32 c = 0; c < 6; c++) {
            const u32 d = dst + 4 * (i * 8 + c);
            ps_writef32(s.base, d, ps_readf32(s.base, d) + ps_readf32(s.base, src + 4 * (i * 6 + kFrom[c])) * volume);
        }
    }
    return lv2::CELL_OK;
}

// The checks the port queries share (RPCS3): init, a port number below 8, an open port.
s32 open_port_checks(AudioState& s, u32 port_num, const char* fn)
{
    if (!s.init) return fail_polled(s, error(CELL_AUDIO_ERROR_NOT_INIT), "{}({})", fn, port_num);
    if (port_num >= AUDIO_PORT_COUNT) return fail_polled(s, error(CELL_AUDIO_ERROR_PARAM), "{}({})", fn, port_num);
    if (s.ports[port_num].state == PortState::Closed) {
        return fail_polled(s, error(CELL_AUDIO_ERROR_PORT_NOT_OPEN), "{}({})", fn, port_num);
    }
    return lv2::CELL_OK;
}

// cellAudioGetPortTimestamp(portNum, u64 tag, u64* stamp): the guest system time (us) at which the period with this
// tag (a value of the global period counter) ended: the port's last period's time, less a period per tag since
// (RPCS3). A tag the port has not reached is CELL_AUDIO_ERROR_TAG_NOT_FOUND.
s32 cellAudioGetPortTimestamp(PPUContext& ctx, u32 port_num, u64 tag, u32 stamp)
{
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (const s32 r = open_port_checks(s, port_num, "cellAudioGetPortTimestamp"); r != lv2::CELL_OK) return r;
    const Port& port = s.ports[port_num];
    if (port.global_counter < tag) {
        return fail_polled(s, error(CELL_AUDIO_ERROR_TAG_NOT_FOUND), "cellAudioGetPortTimestamp({}, tag {})",
            port_num, tag);
    }
    if (!stamp) return lv2::CELL_OK; // RPCS3: "apparently no error is returned if stamp is null"
    if (!guest_readable(thread_of(ctx), stamp, 8)) {
        log_call_site(ctx, s.base, "cellAudioGetPortTimestamp: the stamp pointer is not in guest memory");
        return error(CELL_AUDIO_ERROR_PARAM);
    }
    ps_write64(s.base, stamp, port.timestamp - (port.global_counter - tag) * kBlockPeriodUs);
    return lv2::CELL_OK;
}

// cellAudioGetPortBlockTag(portNum, u64 blockNo, u64* tag): the period counter value at which block blockNo plays,
// counted from the port's current block (RPCS3: global_counter + blockNo - cur_pos).
s32 cellAudioGetPortBlockTag(PPUContext& ctx, u32 port_num, u64 block_no, u32 tag)
{
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (const s32 r = open_port_checks(s, port_num, "cellAudioGetPortBlockTag"); r != lv2::CELL_OK) return r;
    const Port& port = s.ports[port_num];
    if (block_no >= port.num_blocks) {
        return fail_polled(s, error(CELL_AUDIO_ERROR_PARAM), "cellAudioGetPortBlockTag({}, block {})", port_num,
            block_no);
    }
    if (!tag) return lv2::CELL_OK; // RPCS3: no error for a null tag either
    if (!guest_readable(thread_of(ctx), tag, 8)) {
        log_call_site(ctx, s.base, "cellAudioGetPortBlockTag: the tag pointer is not in guest memory");
        return error(CELL_AUDIO_ERROR_PARAM);
    }
    ps_write64(s.base, tag, port.global_counter + block_no - port.cur_pos);
    return lv2::CELL_OK;
}

// cellAudioSetPortLevel(portNum, float level): the port's level moves to `level` over 624 samples (13 ms). RPCS3
// computes the step as (old - new) / 624, the wrong sign, so its level jumps at the first sample; here it moves as its
// comment says it should. A negative level is accepted and ignored (RPCS3). level in f1.
s32 cellAudioSetPortLevel(PPUContext& ctx, u32 port_num)
{
    const f32 level = static_cast<f32>(ctx.fpr[1]);
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (const s32 r = open_port_checks(s, port_num, "cellAudioSetPortLevel"); r != lv2::CELL_OK) return r;
    Port& port = s.ports[port_num];
    if (!(level >= 0.0f)) {
        PS_LOG_WARN(kChan, "cellAudioSetPortLevel({}, {}): a negative level is ignored (as RPCS3)", port_num, level);
        return lv2::CELL_OK;
    }
    port.level_target = level;
    port.level_inc = (level - port.level) / kLevelSteps;
    if (port.level_inc == 0.0f) port.level = level;
    return lv2::CELL_OK;
}

// cellAudioMiscSetAccessoryVolume(devNum, float volume): no accessories; accepted once initialised (RPCS3).
s32 cellAudioMiscSetAccessoryVolume(PPUContext& ctx, u32 dev)
{
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioMiscSetAccessoryVolume({})", dev);
    PS_LOG_DEBUG(kChan, "cellAudioMiscSetAccessoryVolume({}, {}): no accessory", dev, ctx.fpr[1]);
    return lv2::CELL_OK;
}

// Personal (headset) streams: 4 streams, devices 0 to 4 or the primary one; accepted and not routed (RPCS3).
s32 cellAudioSetPersonalDevice(PPUContext&, s32 stream, s32 device)
{
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioSetPersonalDevice({})", stream);
    if (stream < 0 || stream >= 4 || (device != CELL_AUDIO_PERSONAL_DEVICE_PRIMARY && (device < 0 || device >= 5))) {
        return fail(error(CELL_AUDIO_ERROR_PARAM), "cellAudioSetPersonalDevice({}, {})", stream, device);
    }
    return lv2::CELL_OK;
}

s32 cellAudioUnsetPersonalDevice(PPUContext&, s32 stream)
{
    AudioState& s = audio();
    std::lock_guard lock(s.mutex);
    if (!s.init) return fail(error(CELL_AUDIO_ERROR_NOT_INIT), "cellAudioUnsetPersonalDevice({})", stream);
    if (stream < 0 || stream >= 4) return fail(error(CELL_AUDIO_ERROR_PARAM), "cellAudioUnsetPersonalDevice({})", stream);
    return lv2::CELL_OK;
}

} // namespace

void set_audio_sink(HleProcess& process, AudioSink sink, AudioBufferQuery query, AudioStretch stretch)
{
    AudioState& s = process.state<AudioState>();
    if (!sink) query = {};
    if (!query) stretch = {};
    std::lock_guard lock(s.sink_mutex);
    s.has_sink.store(static_cast<bool>(sink), std::memory_order_release);
    s.buffered.store(static_cast<bool>(query), std::memory_order_release);
    s.sink = std::move(sink);
    s.query = std::move(query);
    s.stretch = std::move(stretch);
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
        PS_HLE_FUNC(0xA4C9BA65, cellAudioCreateNotifyEventQueueEx),
        PS_HLE_FUNC(0xB56EF5A1, cellAudioSetNotifyEventQueueEx),
        PS_HLE_FUNC(0x2B9BD9AD, cellAudioRemoveNotifyEventQueueEx),
        PS_HLE_FUNC(0x17D1213B, cellAudioSendAck),
        PS_HLE_FUNC(0x9E4B1DB8, cellAudioAdd2chData),
        PS_HLE_FUNC(0x832DF17E, cellAudioAdd6chData),
        PS_HLE_FUNC(0x4109D08C, cellAudioGetPortTimestamp),
        PS_HLE_FUNC(0xE4046AFE, cellAudioGetPortBlockTag),
        PS_HLE_FUNC(0x56DFE179, cellAudioSetPortLevel),
        PS_HLE_FUNC(0x31211F6B, cellAudioMiscSetAccessoryVolume),
        PS_HLE_FUNC(0x5676F81C, cellAudioSetPersonalDevice),
        PS_HLE_FUNC(0x28BC1409, cellAudioUnsetPersonalDevice),
    });
}

} // namespace pspocket::hle
