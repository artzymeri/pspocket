#include "pspocket/rsx/rsx.hpp"

#include <chrono>
#include <format>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/core/timebase.h"
#include "pspocket/rsx/gcm.hpp"
#include "pspocket/rsx/method_names.hpp"

namespace pspocket::rsx {

namespace {
constexpr std::string_view kChan = "rsx";
} // namespace

// ---- handler table ----------------------------------------------------------------------------------------------

struct Rsx::HandlerTable {
    std::array<Handler, m::kMethodCount> table{};
    std::array<bool, m::kMethodCount> known{};

    void bind(u32 reg, Handler h) { table[reg] = h; }
    void bind_range(u32 base, u32 count, u32 stride, Handler h)
    {
        for (u32 i = 0; i < count; i++) table[base + i * stride] = h;
    }

    HandlerTable()
    {
        for (u32 r = 0; r < m::kMethodCount; r++) known[r] = is_known_method(r);

        // NV406E (RPCS3 rsx_methods.cpp: bind(NV406E_*, nv406e::*)); SEMAPHORE_ACQUIRE is handled in dispatch().
        bind(m::NV406E_SET_REFERENCE, &Rsx::m_set_reference);
        bind(m::NV406E_SEMAPHORE_RELEASE, &Rsx::m_semaphore_release);

        // NV4097 draws and immediate data
        bind(m::NV4097_SET_BEGIN_END, &Rsx::m_begin_end);
        bind(m::NV4097_DRAW_ARRAYS, &Rsx::m_draw_arrays);
        bind(m::NV4097_DRAW_INDEX_ARRAY, &Rsx::m_draw_index_array);
        bind(m::NV4097_INLINE_ARRAY, &Rsx::m_inline_array);
        bind(m::NV4097_ARRAY_ELEMENT16, &Rsx::m_array_element16);
        bind(m::NV4097_ARRAY_ELEMENT32, &Rsx::m_array_element32);
        bind(m::NV4097_CLEAR_SURFACE, &Rsx::m_clear_surface);
        bind_range(m::NV4097_SET_TRANSFORM_PROGRAM, 32, 1, &Rsx::m_transform_program);
        bind_range(m::NV4097_SET_TRANSFORM_CONSTANT, 32, 1, &Rsx::m_transform_constant);
        bind_range(m::NV4097_SET_VERTEX_DATA_SCALED4S_M, 32, 1, &Rsx::m_vertex_data);
        bind_range(m::NV4097_SET_VERTEX_DATA4UB_M, 16, 1, &Rsx::m_vertex_data);
        bind_range(m::NV4097_SET_VERTEX_DATA1F_M, 16, 1, &Rsx::m_vertex_data);
        bind_range(m::NV4097_SET_VERTEX_DATA2F_M, 32, 1, &Rsx::m_vertex_data);
        bind_range(m::NV4097_SET_VERTEX_DATA3F_M, 64, 1, &Rsx::m_vertex_data);
        bind_range(m::NV4097_SET_VERTEX_DATA4F_M, 64, 1, &Rsx::m_vertex_data);
        bind_range(m::NV4097_SET_VERTEX_DATA2S_M, 16, 1, &Rsx::m_vertex_data);
        bind_range(m::NV4097_SET_VERTEX_DATA4S_M, 32, 1, &Rsx::m_vertex_data);

        // NV4097 sync, reports
        bind(m::NV4097_GET_REPORT, &Rsx::m_get_report);
        bind(m::NV4097_CLEAR_REPORT_VALUE, &Rsx::m_clear_report_value);
        bind(m::NV4097_NOTIFY, &Rsx::m_notify);
        bind(m::NV4097_TEXTURE_READ_SEMAPHORE_RELEASE, &Rsx::m_texture_read_semaphore_release);
        bind(m::NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE, &Rsx::m_back_end_write_semaphore_release);
        bind(m::NV4097_WAIT_FOR_IDLE, &Rsx::m_wait_for_idle);
        bind(m::NV4097_SET_RENDER_ENABLE, &Rsx::m_set_render_enable);
        bind(m::NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE, &Rsx::m_zpass_pixel_count_enable);
        bind(m::NV4097_SET_INDEX_ARRAY_DMA, &Rsx::m_index_array_dma);

        // Dirty tracking (RPCS3 binds set_surface_dirty_bit, set_fragment_texture_dirty_bit, ... to these)
        for (u32 r : {m::NV4097_SET_SURFACE_CLIP_HORIZONTAL, m::NV4097_SET_SURFACE_CLIP_VERTICAL,
                 m::NV4097_SET_SURFACE_FORMAT, m::NV4097_SET_SURFACE_PITCH_A, m::NV4097_SET_SURFACE_PITCH_B,
                 m::NV4097_SET_SURFACE_PITCH_C, m::NV4097_SET_SURFACE_PITCH_D, m::NV4097_SET_SURFACE_PITCH_Z,
                 m::NV4097_SET_SURFACE_COLOR_AOFFSET, m::NV4097_SET_SURFACE_COLOR_BOFFSET,
                 m::NV4097_SET_SURFACE_COLOR_COFFSET, m::NV4097_SET_SURFACE_COLOR_DOFFSET,
                 m::NV4097_SET_SURFACE_ZETA_OFFSET, m::NV4097_SET_SURFACE_COLOR_TARGET,
                 m::NV4097_SET_CONTEXT_DMA_COLOR_A, m::NV4097_SET_CONTEXT_DMA_COLOR_B,
                 m::NV4097_SET_CONTEXT_DMA_COLOR_C, m::NV4097_SET_CONTEXT_DMA_COLOR_D,
                 m::NV4097_SET_CONTEXT_DMA_ZETA, m::NV4097_SET_WINDOW_OFFSET, m::NV4097_SET_CONTROL0,
                 m::NV4097_SET_ANTI_ALIASING_CONTROL}) {
            bind(r, &Rsx::m_dirty_surface);
        }
        for (u32 r : {m::NV4097_SET_TRANSFORM_PROGRAM_START, m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK,
                 m::NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK, m::NV4097_SET_TRANSFORM_PROGRAM_LOAD}) {
            bind(r, &Rsx::m_dirty_vertex_program);
        }
        for (u32 r : {m::NV4097_SET_SHADER_PROGRAM, m::NV4097_SET_SHADER_CONTROL, m::NV4097_INVALIDATE_L2,
                 m::NV4097_SET_SHADER_PACKER, m::NV4097_SET_SHADER_WINDOW, m::NV4097_SET_TWO_SIDE_LIGHT_EN,
                 m::NV4097_SET_POINT_SPRITE_CONTROL}) {
            bind(r, &Rsx::m_dirty_fragment_program);
        }
        bind_range(m::NV4097_SET_TEX_COORD_CONTROL, 10, 1, &Rsx::m_dirty_fragment_program);
        for (u32 r : {m::NV4097_SET_TEXTURE_OFFSET, m::NV4097_SET_TEXTURE_FORMAT, m::NV4097_SET_TEXTURE_ADDRESS,
                 m::NV4097_SET_TEXTURE_CONTROL0, m::NV4097_SET_TEXTURE_CONTROL1, m::NV4097_SET_TEXTURE_FILTER,
                 m::NV4097_SET_TEXTURE_IMAGE_RECT, m::NV4097_SET_TEXTURE_BORDER_COLOR}) {
            bind_range(r, 16, 8, &Rsx::m_dirty_fragment_texture);
        }
        bind_range(m::NV4097_SET_TEXTURE_CONTROL2, 16, 1, &Rsx::m_dirty_fragment_texture);
        bind_range(m::NV4097_SET_TEXTURE_CONTROL3, 16, 1, &Rsx::m_dirty_fragment_texture);
        for (u32 r : {m::NV4097_SET_VERTEX_TEXTURE_OFFSET, m::NV4097_SET_VERTEX_TEXTURE_FORMAT,
                 m::NV4097_SET_VERTEX_TEXTURE_ADDRESS, m::NV4097_SET_VERTEX_TEXTURE_CONTROL0,
                 m::NV4097_SET_VERTEX_TEXTURE_CONTROL3, m::NV4097_SET_VERTEX_TEXTURE_FILTER,
                 m::NV4097_SET_VERTEX_TEXTURE_IMAGE_RECT, m::NV4097_SET_VERTEX_TEXTURE_BORDER_COLOR}) {
            bind_range(r, 4, 8, &Rsx::m_dirty_vertex_texture);
        }
        bind_range(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET, 16, 1, &Rsx::m_dirty_vertex_arrays);
        bind_range(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT, 16, 1, &Rsx::m_dirty_vertex_arrays);
        for (u32 r : {m::NV4097_SET_VERTEX_DATA_BASE_OFFSET, m::NV4097_SET_VERTEX_DATA_BASE_INDEX,
                 m::NV4097_SET_FREQUENCY_DIVIDER_OPERATION}) {
            bind(r, &Rsx::m_dirty_vertex_arrays);
        }

        // 2D engines and memory copies
        bind(m::NV0039_BUFFER_NOTIFY, &Rsx::m_nv0039_buffer_notify);
        bind_range(m::NV308A_COLOR, m::NV308A_COLOR_COUNT, 1, &Rsx::m_nv308a_color);
        bind(m::NV3089_IMAGE_IN, &Rsx::m_nv3089_image_in);

        // lv1 driver methods and the HLE flip command
        bind(m::GCM_FLIP_COMMAND, &Rsx::m_flip_command);
        bind_range(m::GCM_FLIP_HEAD, 2, 1, &Rsx::m_driver_flip);
        bind_range(m::GCM_DRIVER_QUEUE, 8, 1, &Rsx::m_driver_queue);
        bind_range(m::GCM_SET_USER_COMMAND, 2, 1, &Rsx::m_user_command);

        // Valid but meaningless to us (RPCS3 trace_method)
        bind(0x171c >> 2, &Rsx::m_trace);
        bind(0x2100 >> 2, &Rsx::m_trace);
    }
};

const Rsx::HandlerTable& Rsx::handlers()
{
    static const HandlerTable t;
    return t;
}

// ---- lifecycle --------------------------------------------------------------------------------------------------

Rsx::Rsx(GuestMemory& mem, Backend& backend, Config config)
    : mem_(mem), backend_(backend), config_(config)
{
    fifo_location_.store(config_.fifo_location, std::memory_order_release);
    return_stack_.reserve(config_.call_stack_depth);
    (void)handlers();
    backend_.attach(*this);
}

Rsx::~Rsx()
{
    stop();
    backend_.run_deferred(true); // what is queued writes guest memory through this object (D-107)
}

void Rsx::start()
{
    if (thread_.joinable()) return;
    stop_requested_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { thread_main(); });
}

void Rsx::stop()
{
    if (!thread_.joinable()) return;
    stop_requested_.store(true, std::memory_order_release);
    notify();
    thread_.join();
    backend_.run_deferred(true); // the CPU may look at what the RSX wrote last (D-107)
}

void Rsx::notify()
{
    {
        std::lock_guard lock(wake_mutex_);
        wake_pending_ = true;
    }
    wake_cv_.notify_one();
}

u64 Rsx::timestamp_ns()
{
    const unsigned __int128 tb = ps_timebase_now();
    return static_cast<u64>(tb * 1'000'000'000ull / PS_TIMEBASE_HZ);
}

// ---- lv2 / HLE --------------------------------------------------------------------------------------------------

void Rsx::set_fifo_pointers(u32 get, u32 put)
{
    get &= ~3u;
    put &= ~3u;
    // The context's first FIFO setup decides where the push buffer lives (RPCS3 sys_rsx.cpp attribute 0x001).
    FifoLocation expected = FifoLocation::Auto;
    const FifoLocation decided = io_map_.io_to_ea(get) ? FifoLocation::Io : FifoLocation::Local;
    if (fifo_location_.compare_exchange_strong(expected, decided, std::memory_order_acq_rel) &&
        decided == FifoLocation::Local) {
        PS_LOG_INFO(kChan, "FIFO set to unmapped IO offset {:#x}: the push buffer is in local memory", get);
    }

    // Both registers are written at once, as RPCS3 does (RSXThread.cpp, new_get_put). The reset is flagged first and
    // put published after it with release: a reader that sees the new put sees the flag, and stops before it reads a
    // word at the old get against the new put (D-123: up to a batch of stale words past the old put otherwise).
    PS_LOG_INFO(kChan, "FIFO set: get {:#x}, put {:#x} (it was at get {:#x}, put {:#x})", get, put,
        get_.load(std::memory_order_relaxed), read_put());
    pending_get_put_.store((u64{put} << 32) | get, std::memory_order_release);
    fifo_reset_.store(true, std::memory_order_release);
    ps_write32(mem_.base(), config_.layout.get_addr(), get);
    __atomic_store_n(reinterpret_cast<u32*>(mem_.base() + config_.layout.put_addr()), ps_bswap32(put), __ATOMIC_RELEASE);
    if (thread_.joinable() && std::this_thread::get_id() != thread_.get_id()) {
        notify();
        // The caller (an lv2 syscall) returns once the RSX has taken the new pointers.
        while (fifo_reset_.load(std::memory_order_acquire) && !stop_requested_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    }
}

void Rsx::set_display_buffer(u32 id, const DisplayBuffer& buffer)
{
    if (id >= display_buffers_.size()) {
        fail(FailKind::Unsupported, 0, std::format("display buffer id {} out of range", id));
        return;
    }
    std::lock_guard lock(display_mutex_);
    display_buffers_[id] = buffer;
}

std::optional<DisplayBuffer> Rsx::display_buffer(u32 id) const
{
    if (id >= display_buffers_.size()) return std::nullopt;
    std::lock_guard lock(display_mutex_);
    return display_buffers_[id];
}

// ---- address translation ----------------------------------------------------------------------------------------

std::optional<u32> Rsx::translate(u32 offset, u32 location, u32 size) const
{
    const ContextLayout& l = config_.layout;
    switch (location) {
    case loc::kLocal:
    case loc::kDmaFrameBuffer:
        if (offset < config_.local_memory_size && config_.local_memory_size - offset >= size) {
            return PS_MEM_RSX_LOCAL_BASE + offset;
        }
        return std::nullopt;
    case loc::kMain:
    case loc::kDmaHostBuffer: {
        const auto ea = io_map_.io_to_ea(offset);
        if (!ea || (size > 1 && !io_map_.is_mapped(offset, size))) return std::nullopt;
        return ea;
    }
    case loc::kDmaReportLocal:
        if (offset < reports::kReportCount * 16 && reports::kReportCount * 16 - offset >= size) {
            return l.reports_addr + reports::kReportOffset + offset;
        }
        return std::nullopt;
    case loc::kDmaReportMain:
        if (offset < 0x1000000) return io_map_.io_to_ea(0x0e000000 + offset);
        return std::nullopt;
    case loc::kDmaSemaphoreRW:
    case loc::kDmaSemaphoreR:
        if (offset < reports::kSemaphoreCount * 4 && reports::kSemaphoreCount * 4 - offset >= size) {
            return l.reports_addr + offset;
        }
        return std::nullopt;
    case loc::kDmaDeviceRW:
    case loc::kDmaDeviceR:
        if (offset < 0x100000 && 0x100000 - offset >= size) return l.device_addr + offset;
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

std::optional<u32> Rsx::fifo_address(u32 offset) const
{
    if (fifo_location_.load(std::memory_order_acquire) == FifoLocation::Local) {
        if (offset < PS_MEM_RSX_LOCAL_SIZE) return PS_MEM_RSX_LOCAL_BASE + offset;
        return std::nullopt;
    }
    return io_map_.io_to_ea(offset);
}

// The guest writes its commands, then (after a sync) put. Put is read with acquire ordering, so the FIFO words read
// after it are at least as new: on ARM a plain load lets those later loads be satisfied first and see words the
// guest has already replaced (run 048 on the Mac: "malformed FIFO command 0x80000000", D-077).
u32 Rsx::read_put() const
{
    const u32 raw = __atomic_load_n(reinterpret_cast<const u32*>(mem_.base() + config_.layout.put_addr()),
        __ATOMIC_ACQUIRE);
    return ps_bswap32(raw) & ~3u;
}

void Rsx::publish_get() { ps_write32(mem_.base(), config_.layout.get_addr(), get_.load(std::memory_order_relaxed)); }

Rsx::Status Rsx::status() const
{
    Status s;
    s.running = thread_.joinable();
    s.idle = idle_.load(std::memory_order_acquire);
    s.fifo_location = fifo_location_.load(std::memory_order_acquire);
    s.get = get_.load(std::memory_order_acquire);
    s.put = read_put();
    s.words = words_.load(std::memory_order_relaxed);
    s.flips = flips_.load(std::memory_order_relaxed);
    s.failures = failures_.load(std::memory_order_relaxed);
    s.conditional_skips = conditional_skips_.load(std::memory_order_relaxed);
    s.draw_splits = draw_splits_.load(std::memory_order_relaxed);
    s.block = block_reason_.load(std::memory_order_acquire);
    s.block_value = block_value_.load(std::memory_order_relaxed);
    if (s.block == BlockReason::JumpToSelf) {
        // The reason stays set after the guest patches the word, until the FIFO next runs dry: parked only while the
        // word at get is still the jump.
        const auto ea = fifo_address(s.get);
        if (!ea || read32(*ea) != s.block_value) s.block = BlockReason::None;
    }
    if (s.block == BlockReason::Semaphore) {
        // The FIFO thread writes these registers only while it runs commands, not while it waits on the acquire.
        s.semaphore_offset = __atomic_load_n(&state_.regs[m::NV406E_SEMAPHORE_OFFSET], __ATOMIC_RELAXED);
        s.semaphore_context = __atomic_load_n(&state_.regs[m::NV406E_SET_CONTEXT_DMA_SEMAPHORE], __ATOMIC_RELAXED);
        s.semaphore_addr = translate(s.semaphore_offset, s.semaphore_context, 4);
        if (s.semaphore_addr) s.semaphore_value = read32(*s.semaphore_addr);
    }
    return s;
}

// ---- failures ---------------------------------------------------------------------------------------------------

void Rsx::fail(FailKind kind, u32 method, std::string message)
{
    failures_.fetch_add(1, std::memory_order_relaxed);
    const Failure failure{kind, method, get_.load(std::memory_order_relaxed), message};
    if (on_failure) on_failure(failure);

    const bool unsupported = kind == FailKind::Unsupported;
    const FailPolicy policy = unsupported ? config_.unsupported_policy : config_.unhandled_policy;
    if (policy == FailPolicy::Abort) PS_FATAL(kChan, "{}", message);
    if (unsupported) {
        std::lock_guard lock(fail_mutex_);
        if (!reported_.insert(message).second) return; // logged once
    }
    PS_LOG_ERROR(kChan, "{}", message);
}

// What a FIFO error report needs to tell a desync from bad commands: put, the words around get and the last jumps.
std::string Rsx::fifo_context(u32 get, u32 put) const
{
    std::string s = std::format(" (put {:#010x}; words from {:#010x}:", put, get - 16);
    for (u32 o = get - 16; o != get + 20; o += 4) {
        const auto ea = fifo_address(o);
        s += ea ? std::format(" {:08x}", read32(*ea)) : " --------";
    }
    s += "; last jumps:";
    for (u32 k = 0; k < flow_history_.size(); k++) {
        const auto& [from, to] = flow_history_[(flow_next_ + k) % flow_history_.size()];
        if (from || to) s += std::format(" {:#x}->{:#x}", from, to);
    }
    return s + ")";
}

void Rsx::fifo_error(std::string message)
{
    fail(FailKind::FifoError, 0, std::move(message));
    // Recovery (RPCS3 recover_fifo): drop everything up to put.
    get_.store(read_put(), std::memory_order_release);
    packet_remaining_ = 0;
    return_stack_.clear();
    block_reason_ = BlockReason::None;
    replay_end_ = 0;
    publish_get();
}

// The words the reader went through before it lost its place, oldest first, each with what the memory holds now
// where that differs: a changed word was read before the guest finished writing it; an unchanged run shows the
// commands as the reader took them (D-118). A few reports per run, one per desync.
void Rsx::report_desync(std::string_view why, u32 get)
{
    const u64 now = words_.load(std::memory_order_relaxed);
    if (desync_reports_ >= 8 || (desync_reports_ != 0 && now - last_desync_report_ < 4096)) return;
    desync_reports_++;
    last_desync_report_ = now;
    std::string s = std::format("FIFO desync report {} at {:#010x} ({}), {} words after the last jump; the words read, "
                                "oldest first (word!now where the memory differs now):", desync_reports_, get, why,
        words_since_flow_);
    const u32 size = static_cast<u32>(read_history_.size());
    const u32 n = std::min(read_next_, size);
    u32 next = ~0u, changed = 0;
    for (u32 k = 0; k < n; k++) {
        const auto& [at, word] = read_history_[(read_next_ - n + k) % size];
        if (at != next) s += std::format("\n  {:#010x}:", at);
        next = at + 4;
        const auto ea = fifo_address(at);
        const u32 current = ea ? read32(*ea) : word;
        if (current == word) {
            s += std::format(" {:08x}", word);
        } else {
            s += std::format(" {:08x}!{:08x}", word, current);
            changed++;
        }
    }
    s += std::format("\n  then from {:#010x}:", get + 4);
    for (u32 o = get + 4; o != get + 68; o += 4) {
        const auto ea = fifo_address(o);
        s += ea ? std::format(" {:08x}", read32(*ea)) : " --------";
    }
    s += std::format("\n  {} of them changed since they were read", changed);
    PS_LOG_ERROR(kChan, "{}", s);
}

// A desync soon after a jump, call or return is read again from its target a millisecond later: the words there may
// have been read before the guest finished writing them. Two tries per target, then the caller gives up as before.
// The semaphore acquires the reader already passed before the desync point hold during the re-read: their semaphores
// may have moved on since (D-118).
bool Rsx::reread_after_desync(u32 get)
{
    constexpr u64 kWindow = 16384; // words after the target
    const auto& [from, to] = flow_history_[(flow_next_ + flow_history_.size() - 1) % flow_history_.size()];
    if ((from == 0 && to == 0) || words_since_flow_ > kWindow) return false;
    if (to != reread_target_) {
        reread_target_ = to;
        reread_tries_ = 0;
    }
    if (reread_tries_ >= 2) return false;
    reread_tries_++;
    PS_LOG_WARN(kChan, "FIFO desync at {:#010x}, {} words after the jump {:#x} -> {:#x}: reading again from {:#x} "
                       "(try {})", get, words_since_flow_, from, to, to, reread_tries_);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    replay_end_ = std::max(replay_end_, get);
    get_.store(to, std::memory_order_release);
    packet_remaining_ = 0;
    words_since_flow_ = 0;
    desync_suspect_ = false;
    block_reason_ = BlockReason::None;
    publish_get();
    return true;
}

// ---- FIFO -------------------------------------------------------------------------------------------------------

Rsx::RunStatus Rsx::run_until_idle(u64 max_words)
{
    PS_CHECK(!thread_.joinable());
    const RunStatus status = step(max_words);
    if (status != RunStatus::Budget) backend_.run_deferred(true); // what the caller may look at next (D-107)
    return status;
}

Rsx::RunStatus Rsx::step(u64 max_words)
{
    // Deferred readbacks and what follows them (D-107): what the GPU has finished runs first. The thread keeps running
    // them while it waits for commands (wait_for_work), without waiting for the GPU.
    backend_.run_deferred(false);
    return step_commands(max_words);
}

Rsx::RunStatus Rsx::step_commands(u64 max_words)
{
    if (fifo_reset_.load(std::memory_order_acquire)) {
        const u64 gp = pending_get_put_.load(std::memory_order_acquire);
        get_.store(static_cast<u32>(gp), std::memory_order_release);
        packet_remaining_ = 0;
        return_stack_.clear();
        block_reason_ = BlockReason::None;
        fifo_reset_.store(false, std::memory_order_release);
    }

    for (u64 n = 0; n < max_words; n++) {
        u32 get = get_.load(std::memory_order_relaxed);
        const u32 put = read_put();
        // A reset since this batch began: its pointers first, at the next step (D-123).
        if (fifo_reset_.load(std::memory_order_acquire)) return RunStatus::Budget;
        if (get == put) return RunStatus::Idle;
        if (put != epoch_put_) { // the CPU submitted more commands (and wrote what they read before that)
            epoch_put_ = put;
            data_epoch_.fetch_add(1, std::memory_order_release);
        }

        const auto ea = fifo_address(get);
        if (!ea) {
            fifo_error(std::format("FIFO offset {:#010x} is not mapped (put {:#010x})", get, put));
            return RunStatus::Error;
        }
        const u32 word = read32_acquire(*ea);
        read_history_[read_next_++ % read_history_.size()] = {get, word};
        words_since_flow_++;
        if (replay_end_ != 0 && get >= replay_end_) replay_end_ = 0; // past the point of the desync

        if (packet_remaining_ > 0) {
            // Method argument
            if (dispatch(packet_reg_, word) == Dispatch::Blocked) return RunStatus::Blocked;
            words_.fetch_add(1, std::memory_order_relaxed);
            get_.store(get + 4, std::memory_order_release);
            packet_reg_ = (packet_reg_ + packet_inc_) & (m::kMethodCount - 1);
            packet_remaining_--;
            publish_get();
            if (desync_suspect_) { // an unknown method or an impossible primitive (D-118)
                desync_suspect_ = false;
                report_desync("a method the reader cannot have meant", get);
                reread_after_desync(get);
            }
            continue;
        }

        words_.fetch_add(1, std::memory_order_relaxed);
        if (word & cmd::kNonMethodMask) {
            // Flow control (RPCS3 RSXFIFO.cpp FIFO_control::read / thread::run_FIFO)
            const bool old_jump = (word & cmd::kOldJumpMask) == cmd::kOldJump;
            const bool new_jump = (word & cmd::kNewJumpMask) == cmd::kNewJump;
            if (old_jump || new_jump) {
                const u32 target = word & (old_jump ? cmd::kOldJumpOffsetMask : cmd::kNewJumpOffsetMask);
                if (target == get) {
                    // Jump to self: the guest parks the RSX here and patches the word to release it.
                    block_value_ = word;
                    block_reason_ = BlockReason::JumpToSelf;
                    return RunStatus::Blocked;
                }
                note_flow(get, target);
                get_.store(target, std::memory_order_release);
                publish_get();
                continue;
            }
            if ((word & cmd::kCallMask) == cmd::kCall) {
                if (return_stack_.size() >= config_.call_stack_depth) {
                    fifo_error(std::format("FIFO CALL at {:#010x} nested deeper than {} (word {:#010x})", get,
                        config_.call_stack_depth, word));
                    return RunStatus::Error;
                }
                return_stack_.push_back(get + 4);
                note_flow(get, word & cmd::kCallOffsetMask);
                get_.store(word & cmd::kCallOffsetMask, std::memory_order_release);
                publish_get();
                continue;
            }
            if ((word & cmd::kReturnMask) == cmd::kReturn) {
                if (return_stack_.empty()) {
                    fifo_error(std::format("FIFO RETURN at {:#010x} without a CALL", get));
                    return RunStatus::Error;
                }
                note_flow(get, return_stack_.back());
                get_.store(return_stack_.back(), std::memory_order_release);
                return_stack_.pop_back();
                publish_get();
                continue;
            }
            std::string message =
                std::format("malformed FIFO command {:#010x} at {:#010x}{}", word, get, fifo_context(get, put));
            report_desync("a malformed command", get);
            if (config_.unhandled_policy != FailPolicy::Abort && reread_after_desync(get)) {
                fail(FailKind::FifoError, 0, std::move(message));
                continue;
            }
            fifo_error(std::move(message));
            return RunStatus::Error;
        }

        // Method header
        const u32 count = (word >> cmd::kCountShift) & cmd::kCountMask;
        get_.store(get + 4, std::memory_order_release);
        if (count != 0) {
            packet_header_ = word;
            packet_reg_ = (word & cmd::kMethodMask) >> 2;
            packet_inc_ = (word & cmd::kNonIncrementMask) == cmd::kNonIncrement ? 0 : 1;
            packet_remaining_ = count;
        }
        publish_get();
    }
    return RunStatus::Budget;
}

Rsx::Dispatch Rsx::dispatch(u32 reg, u32 value)
{
    if (reg == m::NV406E_SEMAPHORE_ACQUIRE) {
        // Passed already before a desync this re-read goes back over (D-118).
        const bool replayed = replay_end_ != 0 && get_.load(std::memory_order_relaxed) < replay_end_;
        if (!replayed && !semaphore_acquire_satisfied(value)) {
            block_value_ = value;
            block_reason_ = BlockReason::Semaphore;
            return Dispatch::Blocked;
        }
        block_reason_ = BlockReason::None;
        data_epoch_.fetch_add(1, std::memory_order_release); // what its releaser wrote is for the commands after it
    }

    // A vertex constant, the constants' load slot or a vertex base changed between BEGIN and END: what was drawn so
    // far goes first, with the state it saw (RPCS3 common.cpp push_draw_parameter_change, nv4097.cpp
    // set_transform_constant: the draw clause's command barriers).
    if (state_.in_begin_end && !state_.draw.ranges.empty() && splits_draw(reg, value)) [[unlikely]] {
        const Primitive primitive = state_.draw.primitive;
        submit_draw_clause();
        state_.draw.reset(primitive);
        draw_splits_.fetch_add(1, std::memory_order_relaxed);
    }
    state_.regs[reg] = value;
    const HandlerTable& h = handlers();
    if (const Handler handler = h.table[reg]) {
        (this->*handler)(reg, value);
    } else if (!h.known[reg]) {
        desync_suspect_ = true;
        fail(FailKind::UnhandledMethod, reg,
            std::format("unhandled RSX method {} arg {:#010x} (FIFO {:#010x}, header {:#010x})", method_label(reg),
                value, get_.load(std::memory_order_relaxed), packet_header_));
    }
    return Dispatch::Done;
}

bool Rsx::splits_draw(u32 reg, u32 value) const
{
    if (reg >= m::NV4097_SET_TRANSFORM_CONSTANT && reg < m::NV4097_SET_TRANSFORM_CONSTANT + 32) {
        const u32 index = reg - m::NV4097_SET_TRANSFORM_CONSTANT;
        const u32 slot = state_.regs[m::NV4097_SET_TRANSFORM_CONSTANT_LOAD] + index / 4;
        return slot < kTransformConstantCount && std::bit_cast<u32>(state_.transform_constants[slot][index % 4]) != value;
    }
    return (reg == m::NV4097_SET_TRANSFORM_CONSTANT_LOAD || reg == m::NV4097_SET_VERTEX_DATA_BASE_OFFSET ||
               reg == m::NV4097_SET_VERTEX_DATA_BASE_INDEX) &&
        state_.regs[reg] != value;
}

bool Rsx::semaphore_acquire_satisfied(u32 value)
{
    // RPCS3 nv406e.cpp semaphore_acquire
    const u32 offset = state_.regs[m::NV406E_SEMAPHORE_OFFSET];
    const u32 dma = state_.regs[m::NV406E_SET_CONTEXT_DMA_SEMAPHORE];
    const auto ea = translate(offset, dma, 4);
    if (!ea) {
        fail(FailKind::FifoError, m::NV406E_SEMAPHORE_ACQUIRE,
            std::format("NV406E_SEMAPHORE_ACQUIRE: bad semaphore offset {:#x} (dma {:#x})", offset, dma));
        return true;
    }
    // The RSX's own release still queued behind a deferred readback (D-107): the newest one is what its pipeline sees.
    for (auto it = queued_labels_.rbegin(); it != queued_labels_.rend(); ++it) {
        if (it->first == *ea) return it->second == value;
    }
    return read32_acquire(*ea) == value; // what the releaser wrote before the value is visible after it (D-077)
}

// ---- thread -----------------------------------------------------------------------------------------------------

bool Rsx::blocked_condition_cleared()
{
    if (fifo_reset_.load(std::memory_order_acquire) || stop_requested_.load(std::memory_order_acquire)) return true;
    switch (block_reason_.load()) {
    case BlockReason::Semaphore: return semaphore_acquire_satisfied(block_value_);
    case BlockReason::JumpToSelf: {
        const auto ea = fifo_address(get_.load(std::memory_order_relaxed));
        return !ea || read32_acquire(*ea) != block_value_;
    }
    case BlockReason::None: break;
    }
    return read_put() != get_.load(std::memory_order_relaxed);
}

void Rsx::wait_for_work(RunStatus why)
{
    if (why == RunStatus::Idle) block_reason_ = BlockReason::None;
    // Sleep with exponential backoff; never spin (plan section 6 "Threading": busy waits waste a core).
    auto backoff = std::chrono::microseconds(config_.min_wait_us);
    const auto max_backoff = std::chrono::microseconds(config_.max_wait_us);
    while (!blocked_condition_cleared()) {
        backend_.run_deferred(false); // a label the CPU waits for may be behind a deferred readback (D-107)
        std::unique_lock lock(wake_mutex_);
        wake_cv_.wait_for(lock, backoff, [this] { return wake_pending_; });
        wake_pending_ = false;
        lock.unlock();
        backoff = std::min(backoff * 2, max_backoff);
    }
}

void Rsx::thread_main()
{
#if defined(__APPLE__)
    pthread_setname_np("pspocket.rsx");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    while (!stop_requested_.load(std::memory_order_acquire)) {
        const RunStatus status = step(4096);
        if (status == RunStatus::Budget) continue;
        const bool idle = status != RunStatus::Blocked && !fifo_reset_.load(std::memory_order_acquire);
        if (idle) {
            {
                std::lock_guard lock(idle_mutex_);
                idle_.store(true, std::memory_order_release);
            }
            idle_cv_.notify_all();
        }
        wait_for_work(status);
        idle_.store(false, std::memory_order_release);
    }
    {
        std::lock_guard lock(idle_mutex_);
        idle_.store(true, std::memory_order_release);
    }
    idle_cv_.notify_all();
}

bool Rsx::wait_idle(u64 timeout_ns)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns);
    std::unique_lock lock(idle_mutex_);
    while (true) {
        // Idle means: the thread parked on an empty FIFO and put has not moved since.
        if (idle_.load(std::memory_order_acquire) && read_put() == get_.load(std::memory_order_acquire) &&
            !fifo_reset_.load(std::memory_order_acquire)) {
            return true;
        }
        if (idle_cv_.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() +
                                                             std::chrono::milliseconds(1))) ==
                std::cv_status::timeout &&
            std::chrono::steady_clock::now() >= deadline) {
            return idle_.load(std::memory_order_acquire) && read_put() == get_.load(std::memory_order_acquire);
        }
    }
}

} // namespace pspocket::rsx
