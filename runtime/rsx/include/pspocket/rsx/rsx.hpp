#pragma once

// The RSX front end: the command FIFO, method dispatch and state tracking, driven by its own host thread.
//
// Data flow (plan section 7): the guest writes commands into a command buffer (main memory through the IO map, or
// local memory) and advances the put register; the RSX thread reads from get to put, decodes method headers, jumps,
// calls and returns, writes every method argument into the register file and runs the method's handler. Draws,
// clears and flips go to the Backend. Semaphores, labels, the reference register and reports are written to guest
// memory in command order, which is how the game synchronises with the GPU.
//
// Sources: RPCS3 rpcs3/Emu/RSX/RSXFIFO.cpp (parser), RSXThread.cpp (get_address, run_FIFO), rsx_methods.cpp (method
// table), NV47/HW/nv406e.cpp, nv4097.cpp, nv0039.cpp, nv308a.cpp, nv3089.cpp (handlers).

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/types.hpp"
#include "pspocket/rsx/backend.hpp"
#include "pspocket/rsx/context.hpp"
#include "pspocket/rsx/io_map.hpp"
#include "pspocket/rsx/state.hpp"

namespace pspocket::rsx {

enum class FailPolicy : u8 {
    Abort, // log at fatal level and abort (dev builds)
    Log,   // log at error level and carry on (release builds; tests)
};

enum class FailKind : u8 {
    UnhandledMethod, // a method the front end does not know
    FifoError,       // malformed command, unmapped FIFO address, call stack overflow, RETURN without CALL
    Unsupported,     // a known method or state the front end or backend cannot honour yet
};

struct Failure {
    FailKind kind = FailKind::UnhandledMethod;
    u32 method = 0; // register index, if the failure is about a method
    u32 get = 0;    // FIFO offset of the command
    std::string message;
};

// Where FIFO offsets (get/put/jump targets) point. libgcm normally puts the push buffer in IO-mapped main memory;
// GTA IV's libgcm puts it in local memory, which RPCS3 detects at the context's first FIFO setup
// (RSXFIFO.cpp g_fifo_location). Auto does the same: decided by set_fifo_pointers().
enum class FifoLocation : u8 { Auto, Io, Local };

struct Config {
    ContextLayout layout = ContextLayout::rpcs3_default();
    u32 local_memory_size = 0x0F900000; // usable local memory (cellGcmInit, RPCS3 cellGcmSys.cpp)
    FifoLocation fifo_location = FifoLocation::Auto;
    FailPolicy unhandled_policy = FailPolicy::Abort;  // UnhandledMethod and FifoError
    FailPolicy unsupported_policy = FailPolicy::Log;  // Unsupported (logged once per distinct message)
    // NV40 PFIFO keeps a single subroutine return address (a CALL inside a CALL is a hardware error, and RPCS3
    // treats it as one). Raise for experiments; the stack itself is general.
    u32 call_stack_depth = 1;
    // Wait for the GPU before writing labels and the reference register. Off by default: the backend copies every
    // guest resource a command reads at encode time, so in-order writes already mean "consumed" (decision proposed
    // in the RSX report). Turn on to match RPCS3's full pipeline flush.
    bool sync_on_label = false;
    // Idle and semaphore waits sleep with exponential backoff between these bounds (never spin); notify() wakes the
    // thread early.
    u32 min_wait_us = 10;
    u32 max_wait_us = 200;
    // Value written for NV4097_GET_REPORT(ZPASS_PIXEL_CNT) while occlusion queries are not implemented.
    u32 fake_zpass_count = 0xFFFF;
};

class Rsx {
public:
    Rsx(GuestMemory& mem, Backend& backend, Config config = {});
    ~Rsx();
    Rsx(const Rsx&) = delete;
    Rsx& operator=(const Rsx&) = delete;

    // ---- lv2 / HLE integration -----------------------------------------------------------------------------
    IoMap& io_map() { return io_map_; }
    const IoMap& io_map() const { return io_map_; }

    // sys_rsx_context_attribute(0x001): set get and put (FIFO offsets), reset the parser and the call stack.
    // Thread safe; wakes the RSX thread.
    void set_fifo_pointers(u32 get, u32 put);
    FifoLocation fifo_location() const { return fifo_location_.load(std::memory_order_acquire); }

    // cellGcmSetDisplayBuffer (sys_rsx_context_attribute 0x104). id 0..7.
    void set_display_buffer(u32 id, const DisplayBuffer& buffer);
    std::optional<DisplayBuffer> display_buffer(u32 id) const;

    // Wakes the RSX thread to re-check put and any semaphore it is waiting on. Call after writing put or a label
    // from host code (HLE cellGcmFlush, sys_rsx, SPU DMA completion). Guest stores are found by polling anyway.
    void notify();

    // Callbacks into HLE, run on the RSX thread. Set them before start().
    std::function<void(u32 buffer_id)> on_flip;            // after the backend presented the buffer
    std::function<void(u32 head, u32 buffer_id)> on_queue; // GCM_DRIVER_QUEUE (lv1 driver flip queue)
    std::function<void(u32 arg)> on_user_command;          // GCM_SET_USER_COMMAND (user interrupt)
    std::function<void(const Failure&)> on_failure;        // every failure, before an abort

    // ---- running -------------------------------------------------------------------------------------------
    // Starts the RSX thread (QoS userInteractive, plan section 6 "Threading").
    void start();
    // Stops and joins it. Safe to call when not running.
    void stop();
    bool running() const { return thread_.joinable(); }

    enum class RunStatus : u8 {
        Idle,    // get == put
        Blocked, // waiting for something another thread must do: semaphore acquire, jump to self
        Error,   // a FIFO error under FailPolicy::Log (the FIFO was skipped to put)
        Budget,  // max_words processed
    };
    // Drives the FIFO on the calling thread (tests, tools). Must not be used while the thread runs.
    RunStatus run_until_idle(u64 max_words = ~0ull);
    // With the thread running: waits until it is idle (get == put, not blocked). Returns false on timeout.
    bool wait_idle(u64 timeout_ns);

    // ---- inspection and services for the backend ---------------------------------------------------------
    GuestMemory& memory() { return mem_; }
    u8* base() const { return mem_.base(); }
    const Config& config() const { return config_; }
    RsxState& state() { return state_; }
    const RsxState& state() const { return state_; }
    Backend& backend() { return backend_; }

    u32 get() const { return get_.load(std::memory_order_acquire); }
    u32 read_put() const;
    u64 words_processed() const { return words_.load(std::memory_order_relaxed); }
    u64 failure_count() const { return failures_.load(std::memory_order_relaxed); }
    u64 flip_count() const { return flips_.load(std::memory_order_relaxed); }
    u32 call_depth() const { return static_cast<u32>(return_stack_.size()); }

    // Why the FIFO is stopped short of put.
    enum class BlockReason : u8 {
        None,
        Semaphore,  // NV406E_SEMAPHORE_ACQUIRE: the semaphore does not hold the value yet
        JumpToSelf, // a jump to its own address: the guest parks the RSX there and patches the word to release it
    };
    // A snapshot of the FIFO for reports (pspocket-boot's thread report), safe to take from any thread while the RSX
    // thread runs. The semaphore registers are read without synchronisation; they are stable while it is blocked.
    struct Status {
        bool running = false; // the RSX thread exists
        bool idle = false;    // the thread parked on an empty FIFO
        FifoLocation fifo_location = FifoLocation::Auto;
        u32 get = 0;          // FIFO offsets
        u32 put = 0;
        u64 words = 0;        // command words processed
        u64 flips = 0;
        u64 failures = 0;
        u64 conditional_skips = 0; // draws conditional rendering skipped (D-102)
        BlockReason block = BlockReason::None;
        u32 block_value = 0;  // Semaphore: the value it waits for; JumpToSelf: the jump word
        // Semaphore: where the acquire looks (NV406E_SEMAPHORE_OFFSET in the NV406E_SET_CONTEXT_DMA_SEMAPHORE
        // context), its guest address when it translates, and the value there now.
        u32 semaphore_offset = 0;
        u32 semaphore_context = 0;
        std::optional<u32> semaphore_addr;
        u32 semaphore_value = 0;
    };
    // Advances whenever memory the commands read may have changed under them: the CPU moved put (it writes what the
    // commands it submits read before submitting them), a semaphore acquire passed (its releaser wrote before
    // releasing). A backend may trust what it checked during one epoch until the next (D-104).
    u64 data_epoch() const { return data_epoch_.load(std::memory_order_acquire); }

    Status status() const;

    // Guest EA for (offset, location): location is 0/1 (CELL_GCM_LOCATION_LOCAL/MAIN) or a DMA context handle
    // (0xFEED0000 frame buffer, 0xFEED0001 host buffer, semaphore, report, device). nullopt when unmapped or out of
    // range. size > 0 also checks that the whole range is reachable. RPCS3 RSXThread.cpp rsx::get_address.
    std::optional<u32> translate(u32 offset, u32 location, u32 size = 0) const;
    std::optional<u32> fifo_address(u32 offset) const;

    // Failure reporting (the backend uses kind Unsupported). Unsupported failures are logged once per message.
    void fail(FailKind kind, u32 method, std::string message);

    // RSX timestamp in nanoseconds (reports, notifies).
    static u64 timestamp_ns();

private:
    enum class Dispatch : u8 { Done, Blocked };
    using Handler = void (Rsx::*)(u32 reg, u32 value);

    struct HandlerTable;
    static const HandlerTable& handlers();

    // FIFO
    RunStatus step(u64 max_words);
    Dispatch dispatch(u32 reg, u32 value);
    void fifo_error(std::string message);
    void publish_get();
    void thread_main();
    void wait_for_work(RunStatus why);
    bool blocked_condition_cleared();

    // Guest memory helpers
    u32 read32(u32 ea) const { return ps_read32(mem_.base(), ea); }
    // A FIFO word: an acquire load, so the words read after a jump the guest just patched are at least as new as the
    // jump (D-077).
    u32 read32_acquire(u32 ea) const
    {
        return ps_bswap32(__atomic_load_n(reinterpret_cast<const u32*>(mem_.base() + ea), __ATOMIC_ACQUIRE));
    }
    void write32(u32 ea, u32 v) { ps_write32(mem_.base(), ea, v); }
    void write_label(u32 ea, u32 value);

    // Methods (methods.cpp)
    void m_set_reference(u32, u32);
    void m_semaphore_release(u32, u32);
    void m_begin_end(u32, u32);
    void m_draw_arrays(u32, u32);
    void m_draw_index_array(u32, u32);
    void m_inline_array(u32, u32);
    void m_array_element16(u32, u32);
    void m_array_element32(u32, u32);
    void m_clear_surface(u32, u32);
    void m_transform_program(u32, u32);
    void m_transform_constant(u32, u32);
    void m_vertex_data(u32, u32);
    void m_get_report(u32, u32);
    void m_clear_report_value(u32, u32);
    void m_notify(u32, u32);
    void m_texture_read_semaphore_release(u32, u32);
    void m_back_end_write_semaphore_release(u32, u32);
    void m_wait_for_idle(u32, u32);
    void m_set_render_enable(u32, u32);
    void m_zpass_pixel_count_enable(u32, u32);
    void m_index_array_dma(u32, u32);
    void m_dirty_surface(u32, u32);
    void m_dirty_vertex_program(u32, u32);
    void m_dirty_fragment_program(u32, u32);
    void m_dirty_fragment_texture(u32, u32);
    void m_dirty_vertex_texture(u32, u32);
    void m_dirty_pipeline(u32, u32);
    void m_dirty_viewport(u32, u32);
    void m_dirty_vertex_arrays(u32, u32);
    void m_nv0039_buffer_notify(u32, u32);
    void m_nv308a_color(u32, u32);
    void m_nv3089_image_in(u32, u32);
    void m_flip_command(u32, u32);
    void m_driver_flip(u32, u32);
    void m_driver_queue(u32, u32);
    void m_user_command(u32, u32);
    void m_trace(u32, u32);
    bool semaphore_acquire_satisfied(u32 value);
    void do_flip(u32 buffer_id);

    GuestMemory& mem_;
    Backend& backend_;
    Config config_;
    IoMap io_map_;
    RsxState state_;

    // FIFO state. Owned by the thread driving the FIFO; get_ is atomic so other threads can read it.
    std::atomic<u32> get_{0};
    std::atomic<FifoLocation> fifo_location_{FifoLocation::Auto};
    u32 packet_reg_ = 0;
    u32 packet_inc_ = 0;
    u32 packet_remaining_ = 0;
    u32 packet_header_ = 0;
    std::vector<u32> return_stack_;
    // The last FIFO jumps, calls and returns (from, to), for the report of a FIFO error.
    std::array<std::pair<u32, u32>, 8> flow_history_{};
    u32 flow_next_ = 0;
    void note_flow(u32 from, u32 to)
    {
        flow_history_[flow_next_] = {from, to};
        flow_next_ = (flow_next_ + 1) % static_cast<u32>(flow_history_.size());
    }
    std::string fifo_context(u32 get, u32 put) const;
    // Written by the thread driving the FIFO; atomic so status() can read them. The value is stored first.
    std::atomic<BlockReason> block_reason_{BlockReason::None};
    std::atomic<u32> block_value_{0}; // semaphore value, or the jump word
    std::atomic<bool> fifo_reset_{false};
    std::atomic<u64> pending_get_put_{0};

    // Display buffers and flip queue
    mutable std::mutex display_mutex_;
    std::array<std::optional<DisplayBuffer>, 8> display_buffers_{};
    std::array<u32, 8> last_queued_{};

    // Thread
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    bool wake_pending_ = false;
    std::atomic<bool> idle_{true};
    std::mutex idle_mutex_;
    std::condition_variable idle_cv_;

    // Statistics and failure dedup
    std::atomic<u64> words_{0};
    std::atomic<u64> failures_{0};
    std::atomic<u64> flips_{0};
    std::atomic<u64> conditional_skips_{0};
    std::atomic<u64> data_epoch_{0};
    u32 epoch_put_ = ~0u;
    bool render_enabled_ = true; // NV4097_SET_RENDER_ENABLE: false while the report it names was zero (D-102)
    std::mutex fail_mutex_;
    std::unordered_set<std::string> reported_;
};

} // namespace pspocket::rsx
