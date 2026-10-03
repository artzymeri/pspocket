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

Rsx::~Rsx() { stop(); }

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

    // Both registers are written at once, as RPCS3 does (RSXThread.cpp, new_get_put).
    ps_write32(mem_.base(), config_.layout.put_addr(), put);
    ps_write32(mem_.base(), config_.layout.get_addr(), get);
    pending_get_put_.store((u64{put} << 32) | get, std::memory_order_release);
    fifo_reset_.store(true, std::memory_order_release);
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

u32 Rsx::read_put() const { return ps_read32(mem_.base(), config_.layout.put_addr()) & ~3u; }

void Rsx::publish_get() { ps_write32(mem_.base(), config_.layout.get_addr(), get_.load(std::memory_order_relaxed)); }

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

void Rsx::fifo_error(std::string message)
{
    fail(FailKind::FifoError, 0, std::move(message));
    // Recovery (RPCS3 recover_fifo): drop everything up to put.
    get_.store(read_put(), std::memory_order_release);
    packet_remaining_ = 0;
    return_stack_.clear();
    block_reason_ = BlockReason::None;
    publish_get();
}

// ---- FIFO -------------------------------------------------------------------------------------------------------

Rsx::RunStatus Rsx::run_until_idle(u64 max_words)
{
    PS_CHECK(!thread_.joinable());
    return step(max_words);
}

Rsx::RunStatus Rsx::step(u64 max_words)
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
        if (get == put) return RunStatus::Idle;

        const auto ea = fifo_address(get);
        if (!ea) {
            fifo_error(std::format("FIFO offset {:#010x} is not mapped (put {:#010x})", get, put));
            return RunStatus::Error;
        }
        const u32 word = read32(*ea);

        if (packet_remaining_ > 0) {
            // Method argument
            if (dispatch(packet_reg_, word) == Dispatch::Blocked) return RunStatus::Blocked;
            words_.fetch_add(1, std::memory_order_relaxed);
            get_.store(get + 4, std::memory_order_release);
            packet_reg_ = (packet_reg_ + packet_inc_) & (m::kMethodCount - 1);
            packet_remaining_--;
            publish_get();
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
                    block_reason_ = BlockReason::JumpToSelf;
                    block_value_ = word;
                    return RunStatus::Blocked;
                }
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
                get_.store(word & cmd::kCallOffsetMask, std::memory_order_release);
                publish_get();
                continue;
            }
            if ((word & cmd::kReturnMask) == cmd::kReturn) {
                if (return_stack_.empty()) {
                    fifo_error(std::format("FIFO RETURN at {:#010x} without a CALL", get));
                    return RunStatus::Error;
                }
                get_.store(return_stack_.back(), std::memory_order_release);
                return_stack_.pop_back();
                publish_get();
                continue;
            }
            fifo_error(std::format("malformed FIFO command {:#010x} at {:#010x}", word, get));
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
        if (!semaphore_acquire_satisfied(value)) {
            block_reason_ = BlockReason::Semaphore;
            block_value_ = value;
            return Dispatch::Blocked;
        }
        block_reason_ = BlockReason::None;
    }

    state_.regs[reg] = value;
    const HandlerTable& h = handlers();
    if (const Handler handler = h.table[reg]) {
        (this->*handler)(reg, value);
    } else if (!h.known[reg]) {
        fail(FailKind::UnhandledMethod, reg,
            std::format("unhandled RSX method {} arg {:#010x} (FIFO {:#010x}, header {:#010x})", method_label(reg),
                value, get_.load(std::memory_order_relaxed), packet_header_));
    }
    return Dispatch::Done;
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
    return read32(*ea) == value;
}

// ---- thread -----------------------------------------------------------------------------------------------------

bool Rsx::blocked_condition_cleared()
{
    if (fifo_reset_.load(std::memory_order_acquire) || stop_requested_.load(std::memory_order_acquire)) return true;
    switch (block_reason_) {
    case BlockReason::Semaphore: return semaphore_acquire_satisfied(block_value_);
    case BlockReason::JumpToSelf: {
        const auto ea = fifo_address(get_.load(std::memory_order_relaxed));
        return !ea || read32(*ea) != block_value_;
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
