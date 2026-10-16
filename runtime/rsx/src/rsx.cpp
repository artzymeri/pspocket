#include "pspocket/core/chaos.hpp"
#include "pspocket/rsx/rsx.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <cstring>
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

#include "methods_upload.hpp"

namespace pspocket::rsx {

namespace {
constexpr std::string_view kChan = "rsx";
} // namespace

// ---- handler table ----------------------------------------------------------------------------------------------

struct Rsx::HandlerTable {
    std::array<Handler, m::kMethodCount> table{};
    std::array<bool, m::kMethodCount> known{};
    // A method whose handler the guest can see the effect of, or that can block (D-955): the get register is published
    // up to it first, as when every word published it. The others (registers without a handler, program and constant
    // uploads, vertex data, dirty bits, draw ranges) leave it to Rsx::maybe_publish.
    std::array<bool, m::kMethodCount> publishes{};

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
        for (u32 r : {m::NV4097_SET_SHADER_PROGRAM, m::NV4097_SET_SHADER_CONTROL, m::NV4097_SET_SHADER_PACKER,
                 m::NV4097_SET_SHADER_WINDOW, m::NV4097_SET_TWO_SIDE_LIGHT_EN, m::NV4097_SET_POINT_SPRITE_CONTROL}) {
            bind(r, &Rsx::m_dirty_fragment_program);
        }
        bind(m::NV4097_INVALIDATE_L2, &Rsx::m_invalidate_l2);
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

        // D-955: every method with a handler publishes get before it, but these.
        const std::array quiet{&Rsx::m_transform_program, &Rsx::m_transform_constant, &Rsx::m_vertex_data,
            &Rsx::m_draw_arrays, &Rsx::m_draw_index_array, &Rsx::m_inline_array, &Rsx::m_array_element16,
            &Rsx::m_array_element32, &Rsx::m_dirty_surface, &Rsx::m_dirty_vertex_program,
            &Rsx::m_dirty_fragment_program, &Rsx::m_invalidate_l2, &Rsx::m_dirty_fragment_texture,
            &Rsx::m_dirty_vertex_texture, &Rsx::m_dirty_pipeline, &Rsx::m_dirty_viewport, &Rsx::m_dirty_vertex_arrays,
            &Rsx::m_trace};
        for (u32 r = 0; r < m::kMethodCount; r++) {
            publishes[r] = table[r] && std::find(quiet.begin(), quiet.end(), table[r]) == quiet.end();
        }
        publishes[m::NV406E_SEMAPHORE_ACQUIRE] = true; // dispatch() blocks there
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
    if (const char* v = std::getenv("PSPOCKET_FLIP_VSYNC")) flip_pacing_ = std::string_view(v) == "1"; // D-1044
    return_stack_.reserve(config_.call_stack_depth);
    (void)handlers();
    if (const char* e = std::getenv("PSPOCKET_RSX_EARLY_REPORTS"); e && *e && std::strcmp(e, "0") != 0)
        config_.early_reports = true; // D-1021
    // PSPOCKET_RSX_CPU_AB=N[:MASK] (D-953): bit 32 of MASK (all by default) is the FIFO's argument runs (D-955).
    if (const char* f = std::getenv("PSPOCKET_RSX_CPU_AB")) {
        const char* mask = std::strchr(f, ':');
        if (!mask || (std::strtoul(mask + 1, nullptr, 0) & 32)) fifo_ab_period_ = static_cast<u32>(std::max(1, std::atoi(f)));
    }
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
    vblank_cv_.notify_all(); // a flip waiting for its vblank (D-1044)
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

void Rsx::request_flip(u32 head, u32 value)
{
    {
        std::lock_guard lock(flip_requests_mutex_);
        flip_requests_.emplace_back(head, value);
    }
    flip_requested_.store(true, std::memory_order_release);
    notify();
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

    // Both registers at once, as RPCS3 does (RSXThread.cpp new_get_put, sys_rsx.cpp set_rsx_dmactl). A running RSX
    // thread takes them between two words and writes the control registers itself before it reads put again, so it
    // sees neither the new put with the old get (D-123) nor the new get with the old put: put written here, after the
    // flag, could reach it after it had taken the new get, and it ran from there to the old put through whatever lay
    // between (test_gcm_sys under ThreadSanitizer: off the end of the IO map, D-587). Without a thread to take them,
    // or from a method on the RSX's own thread, they are written here, after the flag, and the parser resets at the
    // next step.
    PS_LOG_INFO(kChan, "FIFO set: get {:#x}, put {:#x} (it was at get {:#x}, put {:#x})", get, put,
        get_.load(std::memory_order_relaxed), read_put());
    const u64 get_put = (u64{put} << 32) | get;
    const bool to_thread = thread_.joinable() && std::this_thread::get_id() != thread_.get_id();
    pending_get_put_.store(get_put, std::memory_order_relaxed);
    reset_writes_registers_.store(to_thread, std::memory_order_relaxed);
    fifo_reset_.store(true, std::memory_order_release);
    if (!to_thread) {
        write_get_put(get_put);
        return;
    }
    notify();
    // The caller (an lv2 syscall) returns once the RSX has taken the new pointers.
    while (fifo_reset_.load(std::memory_order_acquire) && !stop_requested_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
}

void Rsx::write_get_put(u64 get_put)
{
    write32_release(config_.layout.get_addr(), static_cast<u32>(get_put));
    write32_release(config_.layout.put_addr(), static_cast<u32>(get_put >> 32));
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

void Rsx::publish_get()
{
    published_get_ = get_.load(std::memory_order_relaxed);
    write32_release(config_.layout.get_addr(), published_get_);
}

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
        if (!ea || read32_acquire(*ea) != s.block_value) s.block = BlockReason::None;
    }
    if (s.block == BlockReason::Semaphore) {
        // As the acquire found them when it blocked: the registers are the FIFO thread's, which may be running
        // commands again by now (D-587).
        s.semaphore_offset = block_semaphore_offset_.load(std::memory_order_relaxed);
        s.semaphore_context = block_semaphore_context_.load(std::memory_order_relaxed);
        s.semaphore_addr = translate(s.semaphore_offset, s.semaphore_context, 4);
        if (s.semaphore_addr) s.semaphore_value = read32_acquire(*s.semaphore_addr); // the CPU writes it meanwhile
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
    if (flip_requested_.exchange(false, std::memory_order_acquire)) {
        std::vector<std::pair<u32, u32>> requests;
        {
            std::lock_guard lock(flip_requests_mutex_);
            requests.swap(flip_requests_);
        }
        for (const auto& [head, value] : requests) m_driver_flip(m::GCM_FLIP_HEAD + (head & 1), value);
    }
    if (fifo_reset_.load(std::memory_order_acquire)) {
        const u64 gp = pending_get_put_.load(std::memory_order_relaxed);
        // From another thread (set_fifo_pointers): the control registers are this thread's to write, before put is
        // read against the new get.
        if (reset_writes_registers_.load(std::memory_order_relaxed)) write_get_put(gp);
        get_.store(static_cast<u32>(gp), std::memory_order_release);
        packet_remaining_ = 0;
        return_stack_.clear();
        block_reason_ = BlockReason::None;
        fifo_reset_.store(false, std::memory_order_release);
    }

    // With the argument runs the get register reaches the guest lazily (D-955), and where the reader stops, unless a
    // reset is waiting (its pointers are the registers' then).
    const bool fast = fifo_fast();
    struct PublishOnExit {
        Rsx& rsx;
        bool on;
        ~PublishOnExit()
        {
            if (on && !rsx.fifo_reset_.load(std::memory_order_acquire)) {
                rsx.get_.store(rsx.get_.load(std::memory_order_relaxed), std::memory_order_release);
                rsx.publish_get();
            }
        }
    } publish_on_exit{*this, fast};
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

        if (packet_remaining_ > 0 && fast) {
            // Method arguments, a run of them (D-955).
            u64 used = 0;
            if (read_arguments(get, *ea, put, word, max_words - n, used) == Dispatch::Blocked) return RunStatus::Blocked;
            n += used - 1; // the run's first word is this one
            continue;
        }
        if (packet_remaining_ > 0) {
            // Method argument
            if (dispatch(packet_reg_, word) == Dispatch::Blocked) return RunStatus::Blocked;
            words_.fetch_add(1, std::memory_order_relaxed);
            // A FIFO error in the method's handler recovered to put (fifo_error): the reader goes on from there, not
            // from the next word with the packet's count run past zero (D-277).
            if (get_.load(std::memory_order_relaxed) != get) continue;
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
        get_.store(get + 4, fast ? std::memory_order_relaxed : std::memory_order_release);
        if (count != 0) {
            packet_header_ = word;
            packet_reg_ = (word & cmd::kMethodMask) >> 2;
            packet_inc_ = (word & cmd::kNonIncrementMask) == cmd::kNonIncrement ? 0 : 1;
            packet_remaining_ = count;
        }
        if (fast) maybe_publish();
        else publish_get();
    }
    return RunStatus::Budget;
}

// The arguments of the packet being read, from `get` on, as far as the reader may go without looking at put again:
// put (when it is ahead), the packet's end, the 1 MB IO page (the next may map elsewhere) and the step's budget. Each
// word is read and recorded as a word at a time was (an acquire load, the read history, the desync checks) and get_
// moves past it, but put and the FIFO's address are looked at once a run (the reset flag after a method the guest can
// see), the words are counted once, and the get register reaches the guest before such a method, else lazily
// (maybe_publish). Program and constant uploads, two thirds of GTA V's words, go to their handlers straight: what
// dispatch() does besides is nothing for them, but splitting a draw at a constant (D-260).
Rsx::Dispatch Rsx::read_arguments(u32 get, u32 ea, u32 put, u32 word, u64 budget, u64& used)
{
    const HandlerTable& h = handlers();
    u64 n = packet_remaining_;
    if (put > get) n = std::min<u64>(n, (put - get) / 4);
    n = std::min<u64>({n, (IoMap::kPageSize - (get & (IoMap::kPageSize - 1))) / 4, std::max<u64>(budget, 1)});
    used = 0;
    bool moved = false;
    for (u64 k = 0; k < n; k++) {
        const u32 at = get + static_cast<u32>(k) * 4;
        if (k) {
            word = read32_acquire(ea + static_cast<u32>(k) * 4);
            read_history_[read_next_++ % read_history_.size()] = {at, word};
            words_since_flow_++;
            if (replay_end_ != 0 && at >= replay_end_) replay_end_ = 0;
        }
        const u32 reg = packet_reg_;
        if (reg - m::NV4097_SET_TRANSFORM_CONSTANT < 32 && !(state_.in_begin_end && !state_.draw.ranges.empty())) {
            state_.regs[reg] = word;
            m_transform_constant(reg, word);
        } else if (reg - m::NV4097_SET_TRANSFORM_PROGRAM < 32) {
            state_.regs[reg] = word;
            m_transform_program(reg, word);
        } else {
            const bool seen = h.publishes[reg];
            if (seen) publish_get();
            if (dispatch(reg, word) == Dispatch::Blocked) { // get stays at the word, as the guest sees it
                words_.fetch_add(used, std::memory_order_relaxed);
                get_.store(at, std::memory_order_release);
                publish_get();
                return Dispatch::Blocked;
            }
            // New FIFO pointers set meanwhile: taken at the next step, as after any word (D-123).
            if (seen && fifo_reset_.load(std::memory_order_acquire)) {
                used++;
                moved = true;
                break;
            }
        }
        used++;
        // A FIFO error in the handler recovered to put (fifo_error): the reader goes on from there (D-277).
        if (get_.load(std::memory_order_relaxed) != at) {
            moved = true;
            break;
        }
        get_.store(at + 4, std::memory_order_relaxed);
        packet_reg_ = (reg + packet_inc_) & (m::kMethodCount - 1);
        packet_remaining_--;
        if (desync_suspect_) { // an unknown method or an impossible primitive (D-118)
            desync_suspect_ = false;
            publish_get();
            report_desync("a method the reader cannot have meant", at);
            reread_after_desync(at);
            moved = true;
            break;
        }
    }
    words_.fetch_add(used, std::memory_order_relaxed);
    if (!moved) maybe_publish();
    return Dispatch::Done;
}

Rsx::Dispatch Rsx::dispatch(u32 reg, u32 value)
{
    if (reg == m::NV406E_SEMAPHORE_ACQUIRE) {
        // Passed already before a desync this re-read goes back over (D-118).
        const bool replayed = replay_end_ != 0 && get_.load(std::memory_order_relaxed) < replay_end_;
        if (!replayed && !semaphore_acquire_satisfied(value)) {
            block_value_ = value;
            block_semaphore_offset_.store(state_.regs[m::NV406E_SEMAPHORE_OFFSET], std::memory_order_relaxed);
            block_semaphore_context_.store(state_.regs[m::NV406E_SET_CONTEXT_DMA_SEMAPHORE], std::memory_order_relaxed);
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
    // An unaligned offset (a desync's word) passes, as a bad one does: ARM's acquire load faults on an unaligned address
    // (D-587). The release ignores one the same way (m_semaphore_release).
    if (!ea || offset % 4) {
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
    if (flip_requested_.load(std::memory_order_acquire)) return true; // lv2's flips run even while the FIFO waits
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
    const BlockReason reason = block_reason_.load(std::memory_order_acquire);
    const u64 t0 = ps_host_now_ns();
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
    // The waits by kind every 5 s (D-1014): GTA V's walk parks on a jump to itself 10 to 20 times a frame (the PPU
    // patches the word when it has more), on a semaphore under once a frame (released by PPU stores: a wait on the
    // semaphore's line through the reservation system ended none of them, measured and left out).
    const u64 t1 = ps_host_now_ns();
    const usize kind = reason == BlockReason::Semaphore ? 1 : reason == BlockReason::JumpToSelf ? 2 : 0;
    waits_[kind]++;
    wait_ns_[kind] += t1 - t0;
    if (!waits_since_ns_) waits_since_ns_ = t1;
    if (t1 - waits_since_ns_ >= 5'000'000'000ull) {
        PS_LOG_INFO(kChan, "waits in {:.1f} s: idle {} ({:.0f} ms), semaphore {} ({:.0f} ms), jump to self {} ({:.0f} ms)",
            static_cast<double>(t1 - waits_since_ns_) / 1e9, waits_[0], static_cast<double>(wait_ns_[0]) / 1e6, waits_[1],
            static_cast<double>(wait_ns_[1]) / 1e6, waits_[2], static_cast<double>(wait_ns_[2]) / 1e6);
        waits_ = {};
        wait_ns_ = {};
        waits_since_ns_ = t1;
    }
}

void Rsx::thread_main()
{
#if defined(__APPLE__)
    pthread_setname_np("pspocket.rsx");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    while (!stop_requested_.load(std::memory_order_acquire)) {
        if (chaos::on(chaos::S11_RsxFifo)) [[unlikely]] chaos::delay(chaos::S11_RsxFifo, get_.load()); // D-1045
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
