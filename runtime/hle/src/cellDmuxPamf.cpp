// cellDmuxPamf: the PAMF core under cellDmux (D-194). On a PS3, libdmuxpamf runs the demultiplexing on an SPU (a SPURS
// task, or its own SPU thread group) and a PPU thread that turns the SPU's events into the game's callbacks. Here the
// SPU's part is a host thread running dmux::PamfDemuxer over the game's memory, and the PPU thread is a real lv2 thread
// ("_dmux_pamf_ppu_thread", as Sony names it) running _pspocket_dmux_pamf_thread, so the game's callbacks run on a
// guest thread, at the priority and with the stack the game asked for, as on the console.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellDmuxPamf.cpp/.h (DmuxPamfContext, dmux_pamf_spu_context, the
// _CellDmuxCoreOp* functions): its checks, sizes, commands, events and the PPU thread's loop. RPCS3 resumes that loop
// after savestates; pspocket has none, so it is a plain loop.
//
// Differences from RPCS3:
// - The core's state is on the host. The game's memory holds what RPCS3's sizes say it does (QueryAttr, QueryEsAttr) and
//   the access unit queues at the addresses libdmuxpamf uses (after its CellDmuxPamfEsHandle and
//   DmuxPamfElementaryStream), so access units are where they would be on a PS3.
// - Without a SPURS instance from the game, libdmuxpamf starts one of its own ("_libdmux_pamf": one SPU at the SPU
//   priority the game asked for), whose SPU thread group takes 256 KB of the default memory container and an SPU.
//   Here an SPU thread group of that name is created and destroyed with the demuxer but never started: it takes the
//   same from the game and fails where the console's would.
// - Only the PAMF core (libdmux's), not the raw elementary stream one other libraries use (0x9728a0e9).
// - Pointers RPCS3 ensure()s are logged and refused (PAMF_ERROR_ARG, D-042).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#include "dmux.hpp"
#include "dmux_pamf.hpp"
#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/object.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle::dmux {
namespace {

constexpr std::string_view kChan = "cellDmuxPamf";

// libdmuxpamf's structures in the game's memory (RPCS3's CHECK_SIZEs): what the game is asked for, and where the
// access unit queues lie.
constexpr u32 kPamfHandleSize = 0x1c, kPamfContextSize = 0x3d880, kPamfEsHandleSize = 0x14, kPamfEsSize = 0x98;

enum class CommandType : u32 {
    enable_es = 0,
    disable_es = 2,
    set_stream = 4,
    release_au = 6,
    flush_es = 8,
    close = 10,
    reset_stream = 12,
    reset_es = 14,
    resume = 16,
};
struct Command { // DmuxPamfCommand
    CommandType type = CommandType::resume;
    u32 stream_id = 0, private_stream_id = 0;
    bool is_avc = false;
    u32 au_queue_buffer = 0, au_queue_buffer_size = 0, au_max_size = 0, au_specific_info_size = 0;
    bool raw_es = false;
    u32 user_data = 0;
    u32 au_addr = 0, au_size = 0;
};
enum class EventType : u32 { au_found, demux_done, fatal_error, close, flush_done, prog_end_code };
struct Event { // DmuxPamfEvent
    EventType type = EventType::demux_done;
    u32 stream_id = 0, private_stream_id = 0, au_addr = 0, au_size = 0, stream_header_size = 0, user_data = 0;
    u64 pts = 0, dts = 0;
    std::array<u8, 0x10> stream_header_buf{};
    bool is_rap = false;
};
struct StreamInfo { // DmuxPamfStreamInfo
    u32 addr = 0, size = 0, user_data = 0;
    bool continuity = false, raw_es = false;
};
enum class SequenceState : u32 { dormant, resetting, running };

struct Core;

struct Es { // DmuxPamfElementaryStream
    Core* core = nullptr;
    u32 handle = 0;    // its CellDmuxPamfEsHandle
    u32 es_handle = 0; // the front end's DmuxEsContext
    u8 this_index = 0;
    u16 stream_id = 0, private_stream_id = 0;
    bool is_avc = false;
    u32 au_queue_buffer = 0, au_max_size = 0, au_specific_info_size = 0;
    std::array<u8, 0x10> au_specific_info{};
    bool reset_next_au = false;
    u32 es_id = 0;
};

// The SPU's part (RPCS3 dmux_pamf_spu_context): commands in, events out, through queues the PPU side shares.
class SpuSide final : public PamfDemuxer {
public:
    explicit SpuSide(Core& core) : core_(core) {}
    void run(); // cellSpursMain()

private:
    Core& core_;
    bool wait_for_au_queue = false, wait_for_event_queue = false, event_queue_was_too_full = false;
    u8 max_enqueued_events = 4; // 4 + 2 per enabled elementary stream
    bool new_stream = false;

    bool get_next_cmd(Command& out);
    bool send_event(const Event& e);
    bool on_au_found(u8 stream_id, u8 private_stream_id, u32 user_data, std::span<const u8> au, u64 pts, u64 dts,
        bool rap, u8 au_specific_info_size, std::array<u8, 0x10> au_specific_info) override;
    bool on_demux_done() override { return !(wait_for_event_queue = !send_event({EventType::demux_done})); }
    void on_fatal_error() override { send_event({EventType::fatal_error}); }
    bool on_flush_done(u8 stream_id, u8 private_stream_id, u32 user_data) override
    {
        Event e{EventType::flush_done};
        e.stream_id = stream_id;
        e.private_stream_id = private_stream_id;
        e.user_data = user_data;
        if (send_event(e)) return true; // does not set wait_for_event_queue (LLE)
        std::this_thread::yield();      // retried at once by the caller
        return false;
    }
    bool on_prog_end() override { return !(wait_for_event_queue = !send_event({EventType::prog_end_code})); }
    void on_au_queue_full() override { wait_for_au_queue = true; }
};

struct Core { // DmuxPamfContext, its CellDmuxPamfHandle, and the SPU side's queues
    u8* base = nullptr;
    u32 handle = 0, context = 0; // CellDmuxPamfHandle, DmuxPamfContext
    u32 dmux_handle = 0;         // the front end's DmuxContext
    bool prog_end_cb = false;
    CoreResource res;
    u32 spu_group = 0; // libdmuxpamf's SPU thread group, never started; 0 with SPURS
    u32 mutex = 0, cond = 0;
    u64 thread_id = 0;
    u32 tls = 0;

    // Under `mutex` (the lv2 mutex), as in RPCS3.
    u64 au_released_bitset = 0;
    bool stream_reset_requested = false;
    SequenceState sequence_state = SequenceState::dormant;
    s32 max_enabled_es_num = DMUX_PAMF_MAX_ENABLED_ES_NUM, enabled_es_num = 0;
    std::array<Es*, DMUX_PAMF_MAX_ENABLED_ES_NUM> es{};
    u64 user_data = 0;
    bool is_raw_es = false;
    u32 next_es_id = 0;

    // The queues between the PPU side and the SPU side (CellSpursQueues on LLE), under `qm`.
    std::mutex qm;
    std::condition_variable spu_cv;  // the SPU side waits for a command
    lv2::WaitQueue ppu_waiters;      // guest threads waiting for a command's result or an event
    std::optional<Command> cmd;      // one at a time
    std::optional<u32> cmd_result;
    std::optional<StreamInfo> stream_info;
    std::deque<Event> events;        // 4 + 2 * 64 at most
    std::atomic<bool> abort{false};
    std::unique_ptr<SpuSide> spu;
    std::thread spu_thread;

    // The PPU thread's locals (RPCS3 keeps them in the context for savestates).
    u64 au_queue_full_bitset = 0;
    bool stream_reset_started = false, stream_reset_in_progress = false;
    bool sending_fatal_err = false;

    std::map<u32, std::unique_ptr<Es>> es_storage; // by its CellDmuxPamfEsHandle

    void stop_spu_side()
    {
        {
            std::lock_guard lock(qm);
            abort = true;
        }
        spu_cv.notify_all();
        if (spu_thread.joinable()) spu_thread.join();
    }
    ~Core() { stop_spu_side(); }

    // Wakes the guest threads waiting on the queues (under qm).
    void wake_ppu()
    {
        for (lv2::Waiter* w : ppu_waiters.take_all(lv2::SYS_SYNC_FIFO)) lv2::wake(*w, lv2::CELL_OK);
    }

    Es* find_es(u16 stream_id, u16 private_stream_id)
    {
        const bool video = stream_id_to_type_channel(stream_id, private_stream_id).first == STREAM_TYPE_INDEX_VIDEO;
        for (auto it = es.rbegin(); it != es.rend(); ++it)
            if (*it && (*it)->stream_id == stream_id && (video || (*it)->private_stream_id == private_stream_id))
                return *it;
        return nullptr;
    }
};

// Every demuxer of the process, by its CellDmuxPamfHandle; the elementary streams by their CellDmuxPamfEsHandle.
struct Cores {
    std::mutex mutex;
    std::map<u32, std::unique_ptr<Core>> by_handle;
    std::map<u32, Es*> es_by_handle;
    ~Cores()
    {
        for (auto& [h, c] : by_handle) c->stop_spu_side();
    }
};

Cores& cores()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellDmuxPamf called without an HLE process");
    return p->state<Cores>();
}

Core* core_of(u32 handle)
{
    Cores& c = cores();
    std::lock_guard lock(c.mutex);
    const auto it = c.by_handle.find(handle);
    return it == c.by_handle.end() ? nullptr : it->second.get();
}

Es* es_of(u32 handle)
{
    Cores& c = cores();
    std::lock_guard lock(c.mutex);
    const auto it = c.es_by_handle.find(handle);
    return it == c.es_by_handle.end() ? nullptr : it->second;
}

// ---- the SPU side

bool SpuSide::get_next_cmd(Command& out)
{
    std::unique_lock lock(core_.qm);
    if (core_.cmd) {
        out = *core_.cmd;
        core_.cmd.reset();
        return true;
    }
    if ((new_stream || has_work()) && !wait_for_au_queue && !wait_for_event_queue) return false; // go on demuxing
    core_.spu_cv.wait(lock, [&] { return core_.cmd.has_value() || core_.abort.load(); });
    if (core_.abort) return false;
    out = *core_.cmd;
    core_.cmd.reset();
    return true;
}

bool SpuSide::send_event(const Event& e)
{
    std::lock_guard lock(core_.qm);
    if (core_.events.size() >= max_enqueued_events) return false;
    core_.events.push_back(e);
    core_.wake_ppu();
    return true;
}

bool SpuSide::on_au_found(u8 stream_id, u8 private_stream_id, u32 user_data, std::span<const u8> au, u64 pts, u64 dts,
    bool rap, u8 au_specific_info_size, std::array<u8, 0x10> au_specific_info)
{
    Event e{EventType::au_found};
    e.stream_id = stream_id;
    e.private_stream_id = private_stream_id;
    e.au_addr = static_cast<u32>(au.data() - core_.base);
    e.pts = pts;
    e.dts = dts;
    e.au_size = static_cast<u32>(au.size());
    e.stream_header_size = au_specific_info_size;
    e.stream_header_buf = au_specific_info;
    e.user_data = user_data;
    e.is_rap = rap;
    return !(wait_for_event_queue = !send_event(e));
}

void SpuSide::run()
{
    Command cmd;
    while (!core_.abort) {
        if (get_next_cmd(cmd)) {
            event_queue_was_too_full = wait_for_event_queue;
            wait_for_event_queue = false;
            wait_for_au_queue = false;
            {
                std::lock_guard lock(core_.qm);
                core_.cmd_result = static_cast<u32>(cmd.type) + 1; // acknowledged before it is carried out (LLE)
                core_.wake_ppu();
            }
            switch (cmd.type) {
            case CommandType::enable_es:
                max_enqueued_events += 2;
                enable_es(cmd.stream_id, cmd.private_stream_id, cmd.is_avc,
                    {core_.base + cmd.au_queue_buffer, cmd.au_queue_buffer_size}, cmd.au_max_size, cmd.raw_es,
                    cmd.user_data);
                break;
            case CommandType::disable_es:
                disable_es(cmd.stream_id, cmd.private_stream_id);
                max_enqueued_events -= 2;
                break;
            case CommandType::set_stream: new_stream = true; break;
            case CommandType::release_au: release_au(cmd.stream_id, cmd.private_stream_id, cmd.au_size); break;
            case CommandType::flush_es: flush_es(cmd.stream_id, cmd.private_stream_id); break;
            case CommandType::close:
                while (!send_event({EventType::close}) && !core_.abort) std::this_thread::yield();
                return;
            case CommandType::reset_stream: reset_stream(); break;
            case CommandType::reset_es:
                reset_es(cmd.stream_id, cmd.private_stream_id, cmd.au_addr ? core_.base + cmd.au_addr : nullptr);
                break;
            case CommandType::resume: break;
            }
        } else if (core_.abort) {
            return;
        }
        // A new stream only once the last is consumed.
        if (new_stream && !has_work()) {
            new_stream = false;
            StreamInfo info;
            {
                std::lock_guard lock(core_.qm);
                if (core_.stream_info) info = *core_.stream_info;
                core_.stream_info.reset();
            }
            set_stream({core_.base + info.addr, info.size}, info.continuity);
            // RPCS3: "prevents White Knight Chronicles II FMVs from freezing, since events are otherwise fired before
            // the game has finished initializing FMV playback".
            std::this_thread::sleep_for(std::chrono::microseconds(1500));
        }
        process_next_pack();
    }
}

// ---- the PPU side

// Parks the guest thread until `ready` holds (under core.qm), as an lv2 wait the kernel can end: a thread the process
// tears down unwinds (ThreadExit).
template <class Ready>
void park_until(lv2::PpuThread& t, Core& c, std::unique_lock<std::mutex>& lk, Ready ready)
{
    while (!ready()) {
        lv2::prepare_wait(t.waiter);
        c.ppu_waiters.push(&t.waiter);
        const lv2::WaitStatus status = lv2::park(t.waiter, lk, 0, {lv2::WaitKind::Sleep, 0}, [&](bool) {
            return c.ppu_waiters.remove(&t.waiter) ? lv2::CancelResult::Removed : lv2::CancelResult::Woken;
        });
        if (status == lv2::WaitStatus::Aborted) throw lv2::ThreadExit{};
        lk.lock();
    }
}

// RPCS3 send_spu_command_and_wait: the caller holds the lv2 mutex until the SPU side has taken the command.
void send_spu_command_and_wait(lv2::PpuThread& t, Core& c, const Command& cmd)
{
    std::unique_lock lk(c.qm);
    if (c.cmd) PS_LOG_ERROR(kChan, "the command queue wasn't empty");
    c.cmd = cmd;
    c.spu_cv.notify_all();
    park_until(t, c, lk, [&] { return c.cmd_result.has_value(); });
    const u32 result = *c.cmd_result;
    c.cmd_result.reset();
    if (result != static_cast<u32>(cmd.type) + 1) PS_LOG_ERROR(kChan, "the SPU side sent an invalid result {}", result);
}

bool lock(lv2::PpuThread& t, u32 mutex) { return lv2::sys_mutex_lock(t, mutex, 0) == lv2::CELL_OK; }
bool unlock(lv2::PpuThread& t, u32 mutex) { return lv2::sys_mutex_unlock(t, mutex) == lv2::CELL_OK; }

// RPCS3 wait_au_released_or_stream_reset.
u32 wait_au_released_or_stream_reset(lv2::PpuThread& t, Core& c, u64 au_queue_full_bitset)
{
    if (!lock(t, c.mutex)) return PAMF_ERROR_FATAL;
    if (au_queue_full_bitset) {
        while (!(au_queue_full_bitset & c.au_released_bitset) && !c.stream_reset_requested) {
            if (lv2::sys_cond_wait(t, c.cond, 0) != lv2::CELL_OK) {
                unlock(t, c.mutex);
                return PAMF_ERROR_FATAL;
            }
        }
    }
    c.stream_reset_started = c.stream_reset_requested;
    c.stream_reset_requested = false;
    c.au_released_bitset = 0;
    return unlock(t, c.mutex) ? PAMF_OK : PAMF_ERROR_FATAL;
}

// RPCS3 set_au_reset.
u32 set_au_reset(lv2::PpuThread& t, Core& c, bool reset)
{
    if (!lock(t, c.mutex)) return PAMF_ERROR_FATAL;
    for (Es* e : c.es)
        if (e) e->reset_next_au = reset;
    return unlock(t, c.mutex) ? PAMF_OK : PAMF_ERROR_FATAL;
}

// RPCS3 DmuxPamfContext::exec: the SPU side's events turned into the front end's notifications.
void exec(PPUContext& ctx, Core& c)
{
    lv2::PpuThread& t = thread_of(ctx);
    const auto fatal = [&] { notify_fatal_err(ctx, PAMF_OK, c.dmux_handle); }; // LLE passes CELL_OK
    for (;;) {
        if (c.sending_fatal_err) {
            c.sending_fatal_err = false;
            fatal();
        }
        c.stream_reset_started = false;
        // An elementary stream's access unit queue is full: wait for a release or a stream reset first.
        if (wait_au_released_or_stream_reset(t, c, c.au_queue_full_bitset) != PAMF_OK) fatal();
        Event event;
        {
            std::unique_lock lk(c.qm);
            park_until(t, c, lk, [&] { return !c.events.empty(); });
            event = c.events.front();
        }
        // Demux done: the sequence is resetting, and a stream reset may have been asked for meanwhile.
        if (event.type == EventType::demux_done) {
            if (!lock(t, c.mutex)) fatal();
            c.sequence_state = SequenceState::resetting;
            if (!unlock(t, c.mutex)) fatal();
            if (!c.stream_reset_started && wait_au_released_or_stream_reset(t, c, 0) != PAMF_OK) fatal();
        }
        // A stream reset: every enabled elementary stream drops its next access unit.
        if (c.stream_reset_started) {
            c.stream_reset_in_progress = true;
            if (set_au_reset(t, c, true) != PAMF_OK) fatal();
        }
        switch (event.type) {
        case EventType::au_found: {
            if (!lock(t, c.mutex)) {
                c.sending_fatal_err = true;
                continue;
            }
            Es* const es = c.find_es(static_cast<u16>(event.stream_id), static_cast<u16>(event.private_stream_id));
            if (!es || es->es_id != event.user_data) { // not enabled (any more)
                if (!unlock(t, c.mutex)) {
                    c.sending_fatal_err = true;
                    continue;
                }
                break;
            }
            // During a stream reset the access units still in the event queue are not reported; the SPU side drops
            // each stream's from the first of them.
            if (c.stream_reset_in_progress) {
                if (es->reset_next_au) {
                    Command cmd{CommandType::reset_es};
                    cmd.stream_id = event.stream_id;
                    cmd.private_stream_id = event.private_stream_id;
                    cmd.au_addr = event.au_addr;
                    send_spu_command_and_wait(t, c, cmd);
                    es->reset_next_au = false;
                }
                if (!unlock(t, c.mutex)) {
                    c.sending_fatal_err = true;
                    continue;
                }
                break;
            }
            AuInfo au;
            au.addr = event.au_addr;
            au.size = event.au_size;
            au.pts = event.pts;
            au.dts = event.dts;
            au.user_data = c.user_data;
            au.specific_info_size = es->au_specific_info_size;
            au.is_rap = event.is_rap;
            if (!c.is_raw_es &&
                stream_id_to_type_channel(static_cast<u16>(event.stream_id), static_cast<u16>(event.private_stream_id))
                        .first == STREAM_TYPE_INDEX_LPCM) {
                es->au_specific_info[0] = event.stream_header_buf[0] >> 4; // channel assignment
                es->au_specific_info[1] = event.stream_header_buf[0] & 0xf; // sampling frequency
                es->au_specific_info[2] = event.stream_header_buf[1] >> 6;  // bits per sample
            }
            au.specific_info = es->au_specific_info;
            const u32 es_handle = es->es_handle;
            const u8 index = es->this_index;
            if (!unlock(t, c.mutex)) {
                c.sending_fatal_err = true;
                continue;
            }
            if (notify_es_au_found(ctx, au, es_handle) != 0) {
                // Its access unit queue is full: wait for a release, then report the same unit again.
                c.au_queue_full_bitset |= 1ull << index;
                continue;
            }
            break;
        }
        case EventType::demux_done:
            if (c.stream_reset_in_progress) {
                c.stream_reset_in_progress = false;
                if (set_au_reset(t, c, false) != PAMF_OK) {
                    c.sending_fatal_err = true;
                    continue;
                }
            }
            notify_demux_done(ctx, c.dmux_handle);
            if (!lock(t, c.mutex)) {
                c.sending_fatal_err = true;
                continue;
            }
            if (c.sequence_state == SequenceState::resetting) {
                c.sequence_state = SequenceState::dormant;
                if (lv2::sys_cond_signal_all(t, c.cond) != lv2::CELL_OK) {
                    unlock(t, c.mutex);
                    c.sending_fatal_err = true;
                    continue;
                }
            }
            if (!unlock(t, c.mutex)) {
                c.sending_fatal_err = true;
                continue;
            }
            break;
        case EventType::close: {
            std::lock_guard lk(c.qm);
            c.events.clear();
            return;
        }
        case EventType::flush_done: {
            if (!lock(t, c.mutex)) {
                c.sending_fatal_err = true;
                continue;
            }
            Es* const es = c.find_es(static_cast<u16>(event.stream_id), static_cast<u16>(event.private_stream_id));
            const bool valid = es && es->es_id == event.user_data;
            const u32 es_handle = valid ? es->es_handle : 0;
            if (!unlock(t, c.mutex)) {
                c.sending_fatal_err = true;
                continue;
            }
            if (valid) notify_es_flush_done(ctx, es_handle);
            break;
        }
        case EventType::prog_end_code:
            if (c.prog_end_cb) notify_prog_end_code(ctx, c.dmux_handle);
            break;
        case EventType::fatal_error: {
            std::lock_guard lk(c.qm);
            c.events.pop_front();
            c.sending_fatal_err = true;
            continue;
        }
        }
        std::size_t remaining = 0;
        {
            std::lock_guard lk(c.qm);
            if (!c.events.empty()) c.events.pop_front();
            remaining = c.events.size();
        }
        // The SPU side stops when too many events wait; once two are left, it is told to go on.
        if (c.enabled_es_num >= 0 && remaining == 2) {
            if (!lock(t, c.mutex)) {
                c.sending_fatal_err = true;
                continue;
            }
            if (c.enabled_es_num >= 0) send_spu_command_and_wait(t, c, {CommandType::resume});
            if (!unlock(t, c.mutex)) {
                c.sending_fatal_err = true;
                continue;
            }
        }
        c.au_queue_full_bitset = 0;
    }
}

// The access unit queue's sizes (RPCS3 dmuxPamfGet*).
u32 au_specific_info_size(u16 stream_id, u16 private_stream_id, bool is_avc)
{
    switch (stream_id_to_type_channel(stream_id, private_stream_id).first) {
    case STREAM_TYPE_INDEX_VIDEO: return is_avc ? 4 : 0; // LLE: four for AVC, though its struct has one reserved field
    case STREAM_TYPE_INDEX_LPCM:
    case STREAM_TYPE_INDEX_AC3: return 3; // LLE: three for AC3 too
    default: return 0;
    }
}

u32 au_queue_max_size(u16 stream_id, u16 private_stream_id)
{
    switch (stream_id_to_type_channel(stream_id, private_stream_id).first) {
    case STREAM_TYPE_INDEX_LPCM: return 0x100;
    case STREAM_TYPE_INDEX_VIDEO:
    case STREAM_TYPE_INDEX_AC3:
    case STREAM_TYPE_INDEX_ATRACX:
    case STREAM_TYPE_INDEX_USER_DATA: return 0x40;
    default: return 0;
    }
}

constexpr u32 CELL_DMUX_PAMF_FS_48K = 48000, CELL_DMUX_PAMF_M2V_MP_ML = 1, CELL_DMUX_PAMF_M2V_MP_HL = 3,
              DMUX_PAMF_LPCM_FRAMES_PER_SEC = 200;
bool avc_level_ok(u32 level) { return level == 21 || level == 30 || level == 31 || level == 32 || level == 41 || level == 42; }

u32 lpcm_au_size(u8* base, u32 info)
{
    const u32 freq = ps_read32(base, info), nch = ps_read32(base, info + 4), bps = ps_read32(base, info + 8);
    return freq * bps / 8 * (nch + (nch & 1)) / DMUX_PAMF_LPCM_FRAMES_PER_SEC; // an odd channel count has a dummy one
}

// RPCS3 dmuxPamfVerifyEsSpecificInfo (5: invalid, "inconsistent with how it's used elsewhere").
u32 verify_es_specific_info(u8* base, u16 stream_id, u16 private_stream_id, bool is_avc, u32 info)
{
    if (!info) return 0;
    switch (stream_id_to_type_channel(stream_id, private_stream_id).first) {
    case STREAM_TYPE_INDEX_VIDEO:
        if (is_avc) return avc_level_ok(ps_read32(base, info)) ? 0 : 5;
        return ps_read32(base, info) > CELL_DMUX_PAMF_M2V_MP_HL ? 5 : 0;
    case STREAM_TYPE_INDEX_LPCM: {
        const u32 freq = ps_read32(base, info), nch = ps_read32(base, info + 4), bps = ps_read32(base, info + 8);
        return freq != CELL_DMUX_PAMF_FS_48K || (nch != 1 && nch != 2 && nch != 6 && nch != 8) || (bps != 16 && bps != 24) ? 5 : 0;
    }
    case STREAM_TYPE_INDEX_AC3:
    case STREAM_TYPE_INDEX_ATRACX:
    case STREAM_TYPE_INDEX_USER_DATA: return 0;
    default: return 5;
    }
}

u32 au_queue_buffer_size(u8* base, u16 stream_id, u16 private_stream_id, bool is_avc, u32 info)
{
    switch (stream_id_to_type_channel(stream_id, private_stream_id).first) {
    case STREAM_TYPE_INDEX_VIDEO:
        if (is_avc) {
            if (!info) return 0x46a870;
            switch (ps_read32(base, info)) {
            case 21: return 0xb00c0;
            case 30: return 0x19f2e0;
            case 31: return 0x260120;
            case 32: return 0x35f6c0;
            case 41: return 0x45e870;
            default: return 0x46a870;
            }
        }
        return info && ps_read32(base, info) > CELL_DMUX_PAMF_M2V_MP_ML ? 0x255000 : 0x70000;
    case STREAM_TYPE_INDEX_LPCM: {
        if (!info) return 0x104380;
        const u32 nch = ps_read32(base, info + 4), au = lpcm_au_size(base, info);
        if (ps_read32(base, info) <= 96000) {
            if (nch > 0 && nch <= 2) return 0x20000 + au;
            if (nch <= 6) return 0x60000 + au;
            if (nch <= 8) return 0x80000 + au;
            return au;
        }
        if (nch > 0 && nch <= 2) return 0x60000 + au;
        if (nch <= 6) return 0x100000 + au;
        return au;
    }
    case STREAM_TYPE_INDEX_AC3: return 0xa000;
    case STREAM_TYPE_INDEX_ATRACX: return 0x6400;
    case STREAM_TYPE_INDEX_USER_DATA: return 0x160000;
    default: return 0;
    }
}

u32 es_mem_size(u8* base, u16 stream_id, u16 private_stream_id, bool is_avc, u32 info)
{
    return au_specific_info_size(stream_id, private_stream_id, is_avc) * au_queue_max_size(stream_id, private_stream_id) +
           au_queue_buffer_size(base, stream_id, private_stream_id, is_avc, info) + 0x7f + kPamfEsSize + 0xf;
}

u32 au_max_size(u8* base, u16 stream_id, u16 private_stream_id, bool is_avc, u32 info)
{
    switch (stream_id_to_type_channel(stream_id, private_stream_id).first) {
    case STREAM_TYPE_INDEX_VIDEO:
        if (is_avc) {
            if (!info) return 0xcc000;
            switch (ps_read32(base, info)) {
            case 21: return 0x12900;
            case 30: return 0x25f80;
            case 31: return 0x54600;
            case 32: return 0x78000;
            case 41: return 0xc0000;
            default: return 0xcc000; // 4.2 (others were refused)
            }
        }
        return !info || ps_read32(base, info) > CELL_DMUX_PAMF_M2V_MP_ML ? 0x12a800 : 0x38000;
    case STREAM_TYPE_INDEX_LPCM: return info ? lpcm_au_size(base, info) : 0;
    case STREAM_TYPE_INDEX_AC3: return 0xf00;
    case STREAM_TYPE_INDEX_ATRACX: return 0x1008;
    case STREAM_TYPE_INDEX_USER_DATA: return 0xa0000;
    default: return 0;
    }
}

// An lv2 object's name: eight bytes, zero padded.
void put_name(u8* base, u32 at, std::string_view name)
{
    for (u32 i = 0; i < 8; i++) ps_write8(base, at + i, i < name.size() ? static_cast<u8>(name[i]) : 0);
}

bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (addr && guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("cellDmuxPamf: {} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// The PPU thread (Sony's "_dmux_pamf_ppu_thread"), started joinable at the game's priority and stack size.
u32 create_thread(PPUContext& ctx, Core& c)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    HleProcess& p = *HleProcess::current();
    const u32 entry = p.descriptor_of("cellDmux", NID_dmux_pamf_thread);
    if (!entry) {
        PS_LOG_ERROR(kChan, "the demuxer's thread function was not linked (the game does not import cellDmux)");
        return PAMF_ERROR_FATAL;
    }
    c.tls = p.alloc_tls();
    if (!c.tls) return PAMF_ERROR_FATAL;
    StackVar v(ctx, base, 0x40); // thread ID (u64), ppu_thread_param_t {entry, tls}, the name
    ps_write32(base, v.addr() + 8, entry);
    ps_write32(base, v.addr() + 12, c.tls + kTlsR13Offset);
    std::memcpy(base + v.addr() + 0x10, "_dmux_pamf_ppu_thread", 22);
    s32 r = lv2::_sys_ppu_thread_create(t, v.addr(), v.addr() + 8, c.handle, 0,
        static_cast<s32>(c.res.ppu_thread_priority), c.res.ppu_thread_stack_size, 1 /* joinable */, v.addr() + 0x10);
    if (r == lv2::CELL_OK) r = lv2::sys_ppu_thread_start(t, static_cast<u32>(ps_read64(base, v.addr())));
    if (r != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "the demuxer's thread: {}", lv2::error_name(r));
        p.free_tls(c.tls);
        c.tls = 0;
        return PAMF_ERROR_FATAL;
    }
    c.thread_id = ps_read64(base, v.addr());
    return PAMF_OK;
}

// RPCS3 join_thread: every stream let go, the SPU side closed, the PPU thread joined.
u32 join_thread(PPUContext& ctx, Core& c)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!lock(t, c.mutex)) return PAMF_ERROR_FATAL;
    for (s32 i = 0; i < c.enabled_es_num && i < DMUX_PAMF_MAX_ENABLED_ES_NUM; i++) c.es[static_cast<usize>(i)] = nullptr;
    c.enabled_es_num = -1;
    send_spu_command_and_wait(t, c, {CommandType::close});
    if (!unlock(t, c.mutex)) return PAMF_ERROR_FATAL;
    StackVar status(ctx, t.kernel.base(), 8);
    return lv2::sys_ppu_thread_join(t, static_cast<u32>(c.thread_id), status.addr()) == lv2::CELL_OK ? PAMF_OK : PAMF_ERROR_FATAL;
}

u32 reset_stream(lv2::PpuThread& t, Core& c)
{
    if (!lock(t, c.mutex)) return PAMF_ERROR_FATAL;
    if (c.sequence_state != SequenceState::running) return unlock(t, c.mutex) ? PAMF_OK : PAMF_ERROR_FATAL;
    send_spu_command_and_wait(t, c, {CommandType::reset_stream});
    c.stream_reset_requested = true;
    if (const s32 r = lv2::sys_cond_signal_to(t, c.cond, static_cast<u32>(c.thread_id));
        r != lv2::CELL_OK && r != lv2::err(lv2::CELL_EPERM)) {
        unlock(t, c.mutex);
        return PAMF_ERROR_FATAL;
    }
    return unlock(t, c.mutex) ? PAMF_OK : PAMF_ERROR_FATAL;
}

} // namespace

void _pspocket_dmux_pamf_thread(PPUContext& ctx, u64 core_handle)
{
    Core* c = core_of(static_cast<u32>(core_handle));
    lv2::PpuThread& t = thread_of(ctx);
    if (!c) {
        PS_LOG_ERROR(kChan, "the demuxer's thread started for an unknown demuxer {:#x}", core_handle);
        lv2::_sys_ppu_thread_exit(t, 0);
    }
    exec(ctx, *c);
    lv2::_sys_ppu_thread_exit(t, 0);
}

u32 core_query_attr(PPUContext& ctx, u32 info, PamfAttr& attr)
{
    // RPCS3 _CellDmuxCoreOpQueryAttr.
    if (info && (!usable(ctx, info, 8, "the PAMF specific information") || ps_read32(guest_base(ctx), info) != 8))
        return PAMF_ERROR_ARG;
    attr.max_enabled_es_num = DMUX_PAMF_MAX_ENABLED_ES_NUM;
    attr.version = DMUX_PAMF_VERSION;
    attr.mem_size = kPamfHandleSize + kPamfContextSize + 0xe7b;
    return PAMF_OK;
}

u32 core_open(PPUContext& ctx, u32 info, const CoreResource& resource, const CoreResourceSpurs* spurs, u32 dmux_handle,
    u32& core_handle)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    // RPCS3 _CellDmuxCoreOpOpen, then DmuxPamfContext::open.
    if (info && (!usable(ctx, info, 8, "the PAMF specific information") || ps_read32(base, info) != 8)) return PAMF_ERROR_ARG;
    if (spurs && !spurs->spurs) return PAMF_ERROR_ARG;
    if (!usable(ctx, resource.mem_addr, std::max<u32>(resource.mem_size, kPamfHandleSize), "the demuxer's memory") ||
        (resource.mem_addr & 0xf) || resource.mem_size < kPamfHandleSize)
        return PAMF_ERROR_ARG;
    auto c = std::make_unique<Core>();
    c->base = base;
    c->handle = resource.mem_addr;
    c->dmux_handle = dmux_handle;
    c->prog_end_cb = info && ps_read8(base, info + 4) != 0;
    // The core's own resource: its memory after its CellDmuxPamfHandle.
    c->res = resource;
    c->res.mem_addr = resource.mem_addr + kPamfHandleSize;
    c->res.mem_size = resource.mem_size - kPamfHandleSize;
    core_handle = c->handle;
    const CoreResource& res = c->res;
    if (res.ppu_thread_priority >= 0xc00 || res.ppu_thread_stack_size < 0x1000 || res.spu_thread_priority >= 0x100 ||
        res.num_of_spus != 1 || !res.mem_addr || res.mem_size < kPamfContextSize + 0xe7b)
        return PAMF_ERROR_ARG;
    c->context = (res.mem_addr + 0x7f) & ~0x7fu;

    StackVar v(ctx, base, 0xa0); // mutex ID, cond ID, group ID, the attributes, the group's name
    const u32 mutex_id = v.addr(), cond_id = v.addr() + 4, group_id = v.addr() + 8, mutex_attr = v.addr() + 0x10,
              cond_attr = v.addr() + 0x40, group_attr = v.addr() + 0x60, group_name = v.addr() + 0x70;
    ps_write32(base, mutex_attr + 0x00, lv2::SYS_SYNC_PRIORITY);
    ps_write32(base, mutex_attr + 0x04, lv2::SYS_SYNC_NOT_RECURSIVE);
    ps_write32(base, mutex_attr + 0x08, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    ps_write32(base, mutex_attr + 0x0C, lv2::SYS_SYNC_NOT_ADAPTIVE);
    put_name(base, mutex_attr + 0x20, "_dxpmtx");
    ps_write32(base, cond_attr + 0x00, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    put_name(base, cond_attr + 0x10, "_dxpcnd");
    if (lv2::sys_mutex_create(t, mutex_id, mutex_attr) != lv2::CELL_OK) return PAMF_ERROR_FATAL;
    c->mutex = ps_read32(base, mutex_id);
    if (lv2::sys_cond_create(t, cond_id, c->mutex, cond_attr) != lv2::CELL_OK) {
        lv2::sys_mutex_destroy(t, c->mutex);
        return PAMF_ERROR_FATAL;
    }
    c->cond = ps_read32(base, cond_id);
    // Without SPURS, libdmuxpamf's own SPU thread group (sys_spu_thread_group_attribute: name size, name, type 0).
    if (!spurs) {
        constexpr std::string_view kGroupName = "_libdmux_pamf";
        std::memcpy(base + group_name, kGroupName.data(), kGroupName.size());
        ps_write8(base, group_name + static_cast<u32>(kGroupName.size()), 0);
        ps_write32(base, group_attr + 0x0, static_cast<u32>(kGroupName.size()) + 1);
        ps_write32(base, group_attr + 0x4, group_name);
        ps_write32(base, group_attr + 0x8, 0);
        ps_write32(base, group_attr + 0xc, 0);
        if (const s32 r = lv2::sys_spu_thread_group_create(t, group_id, 1, static_cast<s32>(res.spu_thread_priority), group_attr);
            r != lv2::CELL_OK) {
            PS_LOG_ERROR(kChan, "the demuxer's SPU thread group: {}", lv2::error_name(r));
            lv2::sys_cond_destroy(t, c->cond);
            lv2::sys_mutex_destroy(t, c->mutex);
            return PAMF_ERROR_FATAL;
        }
        c->spu_group = ps_read32(base, group_id);
    }
    c->spu = std::make_unique<SpuSide>(*c);
    Core& core = *c;
    core.spu_thread = std::thread([&core] { core.spu->run(); });
    {
        Cores& all = cores();
        std::lock_guard lock(all.mutex);
        all.by_handle[core.handle] = std::move(c);
    }
    if (const u32 r = create_thread(ctx, core); r != PAMF_OK) { // undone, rather than left running (RPCS3 keeps it)
        core.stop_spu_side();
        if (core.spu_group) lv2::sys_spu_thread_group_destroy(t, core.spu_group);
        lv2::sys_cond_destroy(t, core.cond);
        lv2::sys_mutex_destroy(t, core.mutex);
        Cores& all = cores();
        std::lock_guard lock(all.mutex);
        all.by_handle.erase(core_handle);
        return r;
    }
    return PAMF_OK;
}

u32 core_close(PPUContext& ctx, u32 core_handle)
{
    Core* c = core_of(core_handle);
    if (!c) return PAMF_ERROR_ARG;
    lv2::PpuThread& t = thread_of(ctx);
    if (join_thread(ctx, *c) != PAMF_OK) return PAMF_ERROR_FATAL;
    c->stop_spu_side();
    if (c->tls) HleProcess::current()->free_tls(c->tls);
    if (c->spu_group) lv2::sys_spu_thread_group_destroy(t, c->spu_group);
    const bool destroyed = lv2::sys_cond_destroy(t, c->cond) == lv2::CELL_OK && lv2::sys_mutex_destroy(t, c->mutex) == lv2::CELL_OK;
    Cores& all = cores();
    std::lock_guard lock(all.mutex);
    for (auto it = all.es_by_handle.begin(); it != all.es_by_handle.end();)
        it = it->second->core == c ? all.es_by_handle.erase(it) : std::next(it);
    all.by_handle.erase(core_handle);
    return destroyed ? PAMF_OK : PAMF_ERROR_FATAL;
}

u32 core_reset_stream(PPUContext& ctx, u32 core_handle)
{
    Core* c = core_of(core_handle);
    if (!c) return PAMF_ERROR_ARG;
    return reset_stream(thread_of(ctx), *c);
}

u32 core_set_stream(PPUContext& ctx, u32 core_handle, u32 stream_address, u32 stream_size, bool discontinuity, u64 user_data)
{
    if (!stream_address || stream_size == 0) return PAMF_ERROR_ARG;
    Core* c = core_of(core_handle);
    if (!c || !usable(ctx, stream_address, stream_size, "the stream")) return PAMF_ERROR_ARG;
    lv2::PpuThread& t = thread_of(ctx);
    if (!lock(t, c->mutex)) return PAMF_ERROR_FATAL;
    c->user_data = static_cast<u32>(user_data); // u32 in libdmuxpamf
    bool busy = false;
    {
        std::lock_guard lk(c->qm);
        busy = c->stream_info.has_value(); // the last stream is not consumed yet
        if (!busy) c->stream_info = StreamInfo{stream_address, stream_size, static_cast<u32>(user_data), !discontinuity, false};
    }
    if (busy) return unlock(t, c->mutex) ? PAMF_ERROR_BUSY : PAMF_ERROR_FATAL;
    send_spu_command_and_wait(t, *c, {CommandType::set_stream});
    c->sequence_state = SequenceState::running;
    return unlock(t, c->mutex) ? PAMF_OK : PAMF_ERROR_FATAL;
}

u32 core_release_au(PPUContext& ctx, u32 core_es_handle, u32 au_addr, u32 au_size)
{
    if (!au_addr || au_size == 0) return PAMF_ERROR_ARG;
    Es* e = es_of(core_es_handle);
    if (!e) return PAMF_ERROR_ARG;
    Core& c = *e->core;
    lv2::PpuThread& t = thread_of(ctx);
    if (!lock(t, c.mutex)) return PAMF_ERROR_FATAL;
    Command cmd{CommandType::release_au};
    cmd.au_addr = au_addr;
    cmd.au_size = au_size;
    cmd.stream_id = e->stream_id;
    cmd.private_stream_id = e->private_stream_id;
    send_spu_command_and_wait(t, c, cmd);
    c.au_released_bitset |= 1ull << e->this_index;
    if (const s32 r = lv2::sys_cond_signal_to(t, c.cond, static_cast<u32>(c.thread_id));
        r != lv2::CELL_OK && r != lv2::err(lv2::CELL_EPERM)) {
        unlock(t, c.mutex);
        return PAMF_ERROR_FATAL;
    }
    return unlock(t, c.mutex) ? PAMF_OK : PAMF_ERROR_FATAL;
}

u32 core_query_es_attr(PPUContext& ctx, u32 es_filter_id, u32 es_specific_info, PamfEsAttr& attr)
{
    if (!es_filter_id || !usable(ctx, es_filter_id, 16, "the ES filter")) return PAMF_ERROR_ARG;
    u8* base = guest_base(ctx);
    const u16 stream_id = static_cast<u16>(ps_read32(base, es_filter_id)),
              private_stream_id = static_cast<u16>(ps_read32(base, es_filter_id + 4));
    const bool is_avc = ps_read32(base, es_filter_id + 8) != 0;
    if (es_specific_info && !usable(ctx, es_specific_info, 12, "the ES specific information")) return PAMF_ERROR_ARG;
    // RPCS3 dmuxPamfGetEsAttr, then the CellDmuxPamfEsHandle.
    u32 r = PAMF_OK;
    if (stream_id_to_type_channel(stream_id, private_stream_id).first == STREAM_TYPE_INDEX_INVALID) r = PAMF_ERROR_UNKNOWN_STREAM;
    else if (verify_es_specific_info(base, stream_id, private_stream_id, is_avc, es_specific_info) != 0) r = PAMF_ERROR_ARG;
    else {
        attr.au_queue_max_size = au_queue_max_size(stream_id, private_stream_id);
        attr.mem_size = es_mem_size(base, stream_id, private_stream_id, is_avc, es_specific_info);
        attr.specific_info_size = au_specific_info_size(stream_id, private_stream_id, is_avc);
    }
    attr.mem_size += kPamfEsHandleSize; // added whatever the answer (RPCS3)
    return r;
}

u32 core_enable_es(PPUContext& ctx, u32 core_handle, u32 es_filter_id, u32 mem_addr, u32 mem_size, u32 es_handle,
    u32 es_specific_info, u32& core_es_handle)
{
    Core* c = core_of(core_handle);
    if (!c || !es_filter_id || !mem_addr || mem_size == 0 || !usable(ctx, es_filter_id, 16, "the ES filter")) return PAMF_ERROR_ARG;
    if ((mem_addr & 0xf) || mem_size < kPamfEsHandleSize || !usable(ctx, mem_addr, mem_size, "the stream's memory")) return PAMF_ERROR_ARG;
    if (es_specific_info && !usable(ctx, es_specific_info, 12, "the ES specific information")) return PAMF_ERROR_ARG;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u16 stream_id = static_cast<u16>(ps_read32(base, es_filter_id)),
              private_stream_id = static_cast<u16>(ps_read32(base, es_filter_id + 4));
    const bool is_avc = ps_read32(base, es_filter_id + 8) != 0;
    core_es_handle = mem_addr;
    // RPCS3 DmuxPamfContext::enable_es, its memory after the CellDmuxPamfEsHandle.
    const u32 es_mem = mem_addr + kPamfEsHandleSize, es_mem_left = mem_size - kPamfEsHandleSize;
    if (es_mem_left < es_mem_size(base, stream_id, private_stream_id, is_avc, es_specific_info)) return PAMF_ERROR_ARG;
    const auto type = stream_id_to_type_channel(stream_id, private_stream_id).first;
    if (type == STREAM_TYPE_INDEX_INVALID) return PAMF_ERROR_UNKNOWN_STREAM;
    if (verify_es_specific_info(base, stream_id, private_stream_id, is_avc, es_specific_info) != 0) return PAMF_ERROR_ARG;
    if (!lock(t, c->mutex)) return PAMF_ERROR_FATAL;
    c->is_raw_es = false;
    if (c->enabled_es_num == c->max_enabled_es_num) return unlock(t, c->mutex) ? PAMF_ERROR_NO_MEMORY : PAMF_ERROR_FATAL;
    if (c->find_es(stream_id, private_stream_id)) return unlock(t, c->mutex) ? PAMF_ERROR_ARG : PAMF_ERROR_FATAL; // enabled
    auto e = std::make_unique<Es>();
    e->core = c;
    e->handle = mem_addr;
    e->es_handle = es_handle;
    e->stream_id = stream_id;
    e->private_stream_id = private_stream_id;
    e->is_avc = is_avc;
    const u32 es_struct = (es_mem + 0xf) & ~0xfu;                         // DmuxPamfElementaryStream
    e->au_queue_buffer = (es_struct + kPamfEsSize + 0x7f) & ~0x7fu;       // the access unit queue after it
    e->au_max_size = au_max_size(base, stream_id, private_stream_id, is_avc, es_specific_info);
    e->au_specific_info_size = au_specific_info_size(stream_id, private_stream_id, is_avc);
    Command cmd{CommandType::enable_es};
    cmd.stream_id = stream_id;
    cmd.private_stream_id = private_stream_id;
    cmd.is_avc = is_avc;
    cmd.au_queue_buffer = e->au_queue_buffer;
    cmd.au_queue_buffer_size = au_queue_buffer_size(base, stream_id, private_stream_id, is_avc, es_specific_info);
    cmd.au_max_size = e->au_max_size;
    cmd.au_specific_info_size = e->au_specific_info_size;
    cmd.user_data = c->next_es_id;
    send_spu_command_and_wait(t, *c, cmd);
    u32 index = 0;
    while (index < c->es.size() && c->es[index]) index++; // there is a free slot (checked above)
    e->this_index = static_cast<u8>(index);
    e->es_id = c->next_es_id++;
    c->es[index] = e.get();
    c->enabled_es_num++;
    {
        Cores& all = cores();
        std::lock_guard lock(all.mutex);
        all.es_by_handle[mem_addr] = e.get();
    }
    c->es_storage[mem_addr] = std::move(e);
    return unlock(t, c->mutex) ? PAMF_OK : PAMF_ERROR_FATAL;
}

u32 core_disable_es(PPUContext& ctx, u32 core_es_handle)
{
    Es* e = es_of(core_es_handle);
    if (!e) return PAMF_ERROR_ARG;
    Core& c = *e->core;
    lv2::PpuThread& t = thread_of(ctx);
    if (!lock(t, c.mutex)) return PAMF_ERROR_FATAL;
    if (!c.find_es(e->stream_id, e->private_stream_id)) return unlock(t, c.mutex) ? PAMF_ERROR_ARG : PAMF_ERROR_FATAL;
    Command cmd{CommandType::disable_es};
    cmd.stream_id = e->stream_id;
    cmd.private_stream_id = e->private_stream_id;
    send_spu_command_and_wait(t, c, cmd);
    c.es[e->this_index] = nullptr;
    c.enabled_es_num--;
    c.au_released_bitset |= 1ull << e->this_index;
    {
        Cores& all = cores();
        std::lock_guard lock(all.mutex);
        all.es_by_handle.erase(core_es_handle);
    }
    c.es_storage.erase(core_es_handle);
    if (const s32 r = lv2::sys_cond_signal_to(t, c.cond, static_cast<u32>(c.thread_id));
        r != lv2::CELL_OK && r != lv2::err(lv2::CELL_EPERM)) {
        unlock(t, c.mutex);
        return PAMF_ERROR_FATAL;
    }
    return unlock(t, c.mutex) ? PAMF_OK : PAMF_ERROR_FATAL;
}

u32 core_flush_es(PPUContext& ctx, u32 core_es_handle)
{
    Es* e = es_of(core_es_handle);
    if (!e) return PAMF_ERROR_ARG;
    lv2::PpuThread& t = thread_of(ctx);
    if (!lock(t, e->core->mutex)) return PAMF_ERROR_FATAL;
    Command cmd{CommandType::flush_es};
    cmd.stream_id = e->stream_id;
    cmd.private_stream_id = e->private_stream_id;
    send_spu_command_and_wait(t, *e->core, cmd);
    return unlock(t, e->core->mutex) ? PAMF_OK : PAMF_ERROR_FATAL;
}

u32 core_reset_es(PPUContext& ctx, u32 core_es_handle)
{
    Es* e = es_of(core_es_handle);
    if (!e) return PAMF_ERROR_ARG;
    lv2::PpuThread& t = thread_of(ctx);
    if (!lock(t, e->core->mutex)) return PAMF_ERROR_FATAL;
    Command cmd{CommandType::reset_es};
    cmd.stream_id = e->stream_id;
    cmd.private_stream_id = e->private_stream_id;
    send_spu_command_and_wait(t, *e->core, cmd);
    return unlock(t, e->core->mutex) ? PAMF_OK : PAMF_ERROR_FATAL;
}

u32 core_reset_stream_and_wait_done(PPUContext& ctx, u32 core_handle)
{
    Core* c = core_of(core_handle);
    if (!c) return PAMF_ERROR_ARG;
    lv2::PpuThread& t = thread_of(ctx);
    if (reset_stream(t, *c) != PAMF_OK) return PAMF_ERROR_FATAL;
    if (!lock(t, c->mutex)) return PAMF_ERROR_FATAL;
    while (c->sequence_state != SequenceState::dormant) {
        if (lv2::sys_cond_wait(t, c->cond, 0) != lv2::CELL_OK) {
            unlock(t, c->mutex);
            return PAMF_ERROR_FATAL;
        }
    }
    return unlock(t, c->mutex) ? PAMF_OK : PAMF_ERROR_FATAL;
}

} // namespace pspocket::hle::dmux
