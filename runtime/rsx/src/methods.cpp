// RSX method handlers. Semantics follow RPCS3 rpcs3/Emu/RSX/NV47/HW/nv406e.cpp, nv4097.cpp, nv0039.cpp,
// nv308a.cpp, nv3089.cpp and rsx_methods.cpp (flip, user command, driver flip/queue); each handler names its source.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <format>
#include <vector>

#include "pspocket/core/log.hpp"
#include "pspocket/rsx/gcm.hpp"
#include "pspocket/rsx/method_names.hpp"
#include "pspocket/rsx/rsx.hpp"

#include "methods_upload.hpp"

namespace pspocket::rsx {

namespace {

constexpr std::string_view kChan = "rsx";

// SET_VERTEX_DATA*_M families (RPCS3 nv4097.h set_vertex_data*_m: element type, components, registers per
// attribute).
struct VertexDataFamily {
    u32 base;
    u32 count;
    VertexType type;
    u32 components;
    u32 regs_per_attr;
};

constexpr VertexDataFamily kVertexDataFamilies[] = {
    {m::NV4097_SET_VERTEX_DATA_SCALED4S_M, 32, VertexType::S1, 4, 2},
    {m::NV4097_SET_VERTEX_DATA4UB_M, 16, VertexType::UB, 4, 1},
    {m::NV4097_SET_VERTEX_DATA1F_M, 16, VertexType::F, 1, 1},
    {m::NV4097_SET_VERTEX_DATA2F_M, 32, VertexType::F, 2, 2},
    {m::NV4097_SET_VERTEX_DATA3F_M, 64, VertexType::F, 3, 4}, // 4 registers per attribute, w is padding
    {m::NV4097_SET_VERTEX_DATA4F_M, 64, VertexType::F, 4, 4},
    {m::NV4097_SET_VERTEX_DATA2S_M, 16, VertexType::S32K, 2, 1},
    {m::NV4097_SET_VERTEX_DATA4S_M, 32, VertexType::S32K, 4, 2},
};

float decode_s16(VertexType type, u16 raw)
{
    const s16 v = static_cast<s16>(raw);
    // S1 is signed normalised with the RSX mapping [-32768, 32767] -> [-1, 1] (RPCS3 RSXVertexFetch.glsl).
    if (type == VertexType::S1) return (static_cast<float>(v) + 0.5f) / 32767.5f;
    return static_cast<float>(v);
}

} // namespace

// ---- NV406E -----------------------------------------------------------------------------------------------------

void Rsx::write_label(u32 ea, u32 value)
{
    if (config_.sync_on_label) backend_.sync();
    if (!backend_.deferring()) {
        write32_release(ea, value);
        return;
    }
    // After a deferred readback before it (D-107).
    queued_labels_.emplace_back(ea, value);
    backend_.defer(ea, 4, [this, ea, value] {
        write32_release(ea, value);
        queued_labels_.pop_front();
    });
}

void Rsx::m_set_reference(u32, u32 value)
{
    // RPCS3 nv406e.cpp set_reference: get is written together with ref.
    if (config_.sync_on_label) backend_.sync();
    publish_get();
    backend_.defer(config_.layout.ref_addr(), 4, [this, value] { write32_release(config_.layout.ref_addr(), value); });
}

void Rsx::m_semaphore_release(u32 reg, u32 value)
{
    // RPCS3 nv406e.cpp semaphore_release
    const u32 offset = state_.regs[m::NV406E_SEMAPHORE_OFFSET];
    const u32 dma = state_.regs[m::NV406E_SET_CONTEXT_DMA_SEMAPHORE];
    if (offset % 4) {
        fail(FailKind::Unsupported, reg, std::format("NV406E_SEMAPHORE_RELEASE: unaligned offset {:#x} ignored", offset));
        return;
    }
    const auto ea = translate(offset, dma, 4);
    if (!ea) {
        fifo_error(std::format("NV406E_SEMAPHORE_RELEASE: bad semaphore offset {:#x} (dma {:#x})", offset, dma));
        return;
    }
    // Flip synchronisation quirk: the driver never writes 0 to the device flip semaphore.
    if (*ea == config_.layout.device_addr + 0x30 && value == 0) value = 1;
    write_label(*ea, value);
}

// ---- NV4097 draws -----------------------------------------------------------------------------------------------

void Rsx::m_begin_end(u32, u32 value)
{
    if (backend_.tracing())
        backend_.trace_note(std::format("SET_BEGIN_END {:#x}{} (get {:#x})", value,
            !value && !state_.in_begin_end ? ", no BEGIN before it" : "", get()));
    // RPCS3 nv4097.cpp set_begin_end (upper bits ignored)
    const u32 prim = value & 0xff;
    if (prim != 0) {
        if (prim > static_cast<u32>(Primitive::Polygon)) {
            // As RPCS3: a warning, and the draw keeps the primitive it had (GTA V sends 36 once in a while in the first
            // mission, run 080). On the iPhone the argument was the next method's header (0x41710) shortly before
            // the reader lost its place: a sign of a desync (D-118).
            static std::atomic<u32> warned{0};
            if (warned.fetch_add(1, std::memory_order_relaxed) < 8) {
                PS_LOG_WARN(kChan, "NV4097_SET_BEGIN_END: invalid primitive {} (argument {:#x}): the last one is kept",
                    prim, value);
            }
            desync_suspect_ = true;
            state_.draw.reset(state_.draw.primitive);
        } else {
            state_.draw.reset(static_cast<Primitive>(prim));
        }
        state_.in_begin_end = true;
        return;
    }
    if (!state_.in_begin_end) return;
    state_.in_begin_end = false;
    submit_draw_clause();
}

void Rsx::submit_draw_clause()
{
    const u32 begin_prim = static_cast<u32>(state_.draw.primitive);
    if (begin_prim == 0 || begin_prim > 10 || state_.draw.empty()) {
        if (backend_.tracing())
            backend_.trace_note(std::format("a draw left out: primitive {}, {} (get {:#x})", begin_prim,
                state_.draw.empty() ? "no vertices" : "an invalid primitive", get()));
        return;
    }
    if (!render_enabled_) { // conditional rendering: the report said nothing passed
        conditional_skips_.fetch_add(1, std::memory_order_relaxed);
        backend_.draw_skipped(state_, "conditional rendering: its report counted 0");
        return;
    }
    backend_.draw(state_);
}

void Rsx::m_draw_arrays(u32 reg, u32 value)
{
    // RPCS3 nv4097.cpp draw_arrays; range layout from rsx_decode.h NV4097_DRAW_ARRAYS
    if (!state_.in_begin_end) {
        fail(FailKind::Unsupported, reg, "NV4097_DRAW_ARRAYS outside BEGIN/END ignored");
        return;
    }
    DrawClause& d = state_.draw;
    if (d.command != DrawCommand::None && d.command != DrawCommand::Array) {
        fail(FailKind::Unsupported, reg, "NV4097_DRAW_ARRAYS mixed with another draw command in one BEGIN/END");
        return;
    }
    d.command = DrawCommand::Array;
    const u32 first = value & 0xffffff;
    const u32 count = (value >> 24) + 1;
    if (!d.ranges.empty() && d.ranges.back().first + d.ranges.back().count == first) {
        d.ranges.back().count += count;
    } else {
        d.ranges.push_back({first, count});
    }
}

void Rsx::m_draw_index_array(u32 reg, u32 value)
{
    // RPCS3 nv4097.cpp draw_index_array
    if (!state_.in_begin_end) {
        fail(FailKind::Unsupported, reg, "NV4097_DRAW_INDEX_ARRAY outside BEGIN/END ignored");
        return;
    }
    DrawClause& d = state_.draw;
    if (d.command != DrawCommand::None && d.command != DrawCommand::Indexed) {
        fail(FailKind::Unsupported, reg, "NV4097_DRAW_INDEX_ARRAY mixed with another draw command in one BEGIN/END");
        return;
    }
    d.command = DrawCommand::Indexed;
    const u32 first = value & 0xffffff;
    const u32 count = (value >> 24) + 1;
    if (!d.ranges.empty() && d.ranges.back().first + d.ranges.back().count == first) {
        d.ranges.back().count += count;
    } else {
        d.ranges.push_back({first, count});
    }
}

void Rsx::m_inline_array(u32 reg, u32 value)
{
    // RPCS3 nv4097.cpp draw_inline_array: the words are vertex data in PS3 (big-endian) order.
    if (!state_.in_begin_end) {
        fail(FailKind::Unsupported, reg, "NV4097_INLINE_ARRAY outside BEGIN/END ignored");
        return;
    }
    DrawClause& d = state_.draw;
    if (d.command != DrawCommand::None && d.command != DrawCommand::InlineArray) {
        fail(FailKind::Unsupported, reg, "NV4097_INLINE_ARRAY mixed with another draw command in one BEGIN/END");
        return;
    }
    d.command = DrawCommand::InlineArray;
    d.inline_words.push_back(value);
}

void Rsx::m_array_element16(u32, u32 value)
{
    // RPCS3 nv4097.cpp set_array_element16: two indices per word, low half first.
    if (!state_.in_begin_end) return;
    state_.draw.array_elements.push_back(value & 0xffff);
    state_.draw.array_elements.push_back(value >> 16);
}

void Rsx::m_array_element32(u32, u32 value)
{
    if (!state_.in_begin_end) return;
    state_.draw.array_elements.push_back(value);
}

void Rsx::m_clear_surface(u32, u32 value) { backend_.clear(value, state_); }

// ---- NV4097 vertex program, constants, immediate data ------------------------------------------------------------

// m_transform_program and m_transform_constant: methods_upload.hpp (inline: the FIFO reader calls them straight, D-955).

void Rsx::program_upload_past_end(u32 reg)
{
    fail(FailKind::Unsupported, reg,
        std::format("vertex program upload past instruction {} ignored", kMaxVertexProgramInstructions));
}

void Rsx::m_vertex_data(u32 reg, u32 value)
{
    const VertexDataFamily* fam = nullptr;
    for (const auto& f : kVertexDataFamilies) {
        if (reg >= f.base && reg < f.base + f.count) fam = &f;
    }
    PS_CHECK(fam != nullptr);
    const u32 index = reg - fam->base;
    const u32 attr = index / fam->regs_per_attr;
    const u32 sub = index % fam->regs_per_attr;

    RegisterVertexData& rv = state_.vertex_registers[attr];
    if (rv.size != fam->components || rv.type != fam->type) {
        // A new type for this attribute: unwritten components get their defaults.
        rv.value = {0.f, 0.f, 0.f, 1.f};
    }
    rv.type = fam->type;
    rv.size = static_cast<u8>(fam->components);

    bool completes = false; // the write finishes the attribute (for immediate mode on attribute 0)
    switch (fam->type) {
    case VertexType::F:
        if (sub < fam->components) {
            rv.raw[sub] = value;
            rv.value[sub] = bits_to_float(value);
        }
        completes = sub == fam->components - 1;
        break;
    case VertexType::UB:
        // Byte 0 (the low byte of the argument) is x (RPCS3 stores the word unswapped and reads bytes in order).
        rv.raw[0] = value;
        for (u32 c = 0; c < 4; c++) rv.value[c] = static_cast<float>((value >> (8 * c)) & 0xff) / 255.f;
        completes = true;
        break;
    case VertexType::S1:
    case VertexType::S32K:
        // Two 16-bit components per word, the low half first.
        rv.raw[sub] = value;
        rv.value[2 * sub] = decode_s16(fam->type, static_cast<u16>(value & 0xffff));
        if (2 * sub + 1 < 4) rv.value[2 * sub + 1] = decode_s16(fam->type, static_cast<u16>(value >> 16));
        completes = 2 * sub + 2 >= fam->components;
        break;
    default: break;
    }

    if (state_.in_begin_end && attr == 0 && completes) {
        // Immediate mode: a complete attribute 0 emits a vertex with the current value of every attribute
        // (RPCS3 rsx_vertex_data.cpp push_buffer_vertex_info, "classic OpenGL 1.x behaviour").
        DrawClause& d = state_.draw;
        for (u32 a = 0; a < kVertexAttributeCount; a++) d.immediate[a].push_back(state_.vertex_registers[a].value);
        d.immediate_vertex_count++;
    }
    state_.dirty_bits |= dirty::kVertexArrays;
}

// ---- NV4097 reports, notifies, semaphores -----------------------------------------------------------------------

void Rsx::m_get_report(u32 reg, u32 value)
{
    // RPCS3 nv4097.cpp get_report: {u64 timestamp, u32 value, u32 pad} at the report DMA context.
    const u32 type = value >> 24;
    const u32 offset = value & 0xffffff;
    const auto ea = translate(offset, state_.regs[m::NV4097_SET_CONTEXT_DMA_REPORT], 16);
    if (!ea) {
        fifo_error(std::format("NV4097_GET_REPORT: bad report offset {:#x}", offset));
        return;
    }
    u32 result = 0;
    bool later = false; // the backend writes the value when the GPU has counted
    bool whole = false; // ... and the timestamp and the ready word with it (D-1021)
    switch (type) {
    case reports::kZpassPixelCount:
        later = backend_.report_zpass(*ea + 8, !config_.early_reports);
        whole = later && !config_.early_reports;
        if (!later) {
            fail(FailKind::Unsupported, reg,
                std::format("NV4097_GET_REPORT: this backend does not count occlusion (ZPASS_PIXEL_CNT); reporting {}",
                    config_.fake_zpass_count));
            result = config_.fake_zpass_count;
        }
        break;
    case reports::kZcullStats:
    case reports::kZcullStats1:
    case reports::kZcullStats2:
    case reports::kZcullStats3: result = 0; break;
    default:
        fail(FailKind::Unsupported, reg, std::format("NV4097_GET_REPORT: bad report type {}", type));
        break;
    }
    if (config_.sync_on_label) backend_.sync();
    // The RSX writes a report once its pipeline has reached it, the count with it: GTA V's queries (0xB21AD0) take a
    // report whose pad is 0 as ready and read its count. Written when the method ran and the count frames later, its
    // water read a zero for "ready" and skipped the sea (the black and vanishing sea of 2026-10-16, D-1021). A count
    // the backend writes whole is left to it.
    if (backend_.tracing() && later) { // the frame trace: the count now (D-1028), waiting for the GPU
        backend_.sync();
        for (int i = 0; i < 1000 && backend_.report_pending(*ea + 8); i++) std::this_thread::sleep_for(std::chrono::microseconds(100));
        backend_.trace_note(std::format("GET_REPORT ZPASS at {:#010x}: {} samples (get {:#x})", *ea, read32(*ea + 8), get()));
    }
    if (whole) return;
    backend_.defer(*ea, 16, [this, e = *ea, later, result] {
        ps_write64(base(), e, timestamp_ns());
        if (!later) write32(e + 8, result);
        // PSPOCKET_RSX_EARLY_REPORTS: ready before the count (the alarm, D-1022)
        if (later && backend_.report_pending(e + 8)) alarms_.add(Alarm::ReportEarly);
        write32(e + 12, 0);
    });
}

void Rsx::m_clear_report_value(u32 reg, u32 value)
{
    if (backend_.tracing()) backend_.trace_note(std::format("CLEAR_REPORT_VALUE {} (get {:#x})", value, get()));
    if (value != reports::kZpassPixelCount && value != reports::kZcullStats) {
        fail(FailKind::Unsupported, reg, std::format("NV4097_CLEAR_REPORT_VALUE: bad type {}", value));
    }
    if (value == reports::kZpassPixelCount) backend_.clear_zpass();
}

void Rsx::m_zpass_pixel_count_enable(u32, u32 value)
{
    if (backend_.tracing()) backend_.trace_note(std::format("ZPASS_PIXEL_COUNT_ENABLE {} (get {:#x})", value, get()));
    backend_.set_zpass_counting(value != 0);
}

void Rsx::m_notify(u32, u32)
{
    // RPCS3 nv4097.cpp set_notify: NOTIFY_MAIN_x contexts write {timestamp, 0} at IO 0x0f100000 + index * 0x40.
    const u32 location = state_.regs[m::NV4097_SET_CONTEXT_DMA_NOTIFIES];
    if ((location & ~7u) != (loc::kDmaNotifyMain0 & ~7u)) {
        PS_LOG_TRACE(kChan, "NV4097_NOTIFY: context {:#x} ignored", location);
        return;
    }
    const u32 index = (location & 7) ^ 7;
    const auto ea = io_map_.io_to_ea(0x0f100000 + index * 0x40);
    if (!ea) {
        fifo_error(std::format("NV4097_NOTIFY: notify area {} is not IO mapped", index));
        return;
    }
    if (config_.sync_on_label) backend_.sync();
    backend_.defer(*ea, 16, [this, e = *ea] {
        ps_write64(base(), e, timestamp_ns());
        ps_write64(base(), e + 8, 0);
    });
}

void Rsx::m_texture_read_semaphore_release(u32, u32 value)
{
    // RPCS3 nv4097.cpp texture_read_semaphore_release: 16-byte aligned label in the NV4097 semaphore context.
    const u32 offset = state_.regs[m::NV4097_SET_SEMAPHORE_OFFSET];
    if (offset % 16) {
        fifo_error(std::format("NV4097 semaphore with unaligned offset {:#x}", offset));
        return;
    }
    const auto ea = translate(offset, state_.regs[m::NV4097_SET_CONTEXT_DMA_SEMAPHORE], 4);
    if (!ea) {
        fifo_error(std::format("NV4097_TEXTURE_READ_SEMAPHORE_RELEASE: bad offset {:#x}", offset));
        return;
    }
    write_label(*ea, value);
}

void Rsx::m_back_end_write_semaphore_release(u32, u32 value)
{
    // RPCS3 nv4097.cpp back_end_write_semaphore_release: the value's bytes 0 and 2 are exchanged.
    const u32 offset = state_.regs[m::NV4097_SET_SEMAPHORE_OFFSET];
    if (offset % 16) {
        fifo_error(std::format("NV4097 semaphore with unaligned offset {:#x}", offset));
        return;
    }
    const auto ea = translate(offset, state_.regs[m::NV4097_SET_CONTEXT_DMA_SEMAPHORE], 4);
    if (!ea) {
        fifo_error(std::format("NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE: bad offset {:#x}", offset));
        return;
    }
    write_label(*ea, (value & 0xff00ff00) | ((value & 0xff) << 16) | ((value >> 16) & 0xff));
}

void Rsx::m_wait_for_idle(u32, u32) { backend_.sync(); }

void Rsx::m_set_render_enable(u32 reg, u32 value)
{
    // RPCS3 nv4097.cpp set_render_mode: 1 = render unconditionally, 2 = render if the report at offset is nonzero.
    // The report is read when the method runs: occlusion results arrive once the GPU has counted (D-102). A count the
    // GPU has not delivered yet renders (D-115): what is there is an older count, and a draw it skipped left holes the
    // later passes shaded black or red (the iPhone's slower GPU, characters' clothes). RPCS3 defers the evaluation to
    // the GPU instead.
    const u32 mode = value >> 24;
    if (backend_.tracing()) backend_.trace_note(std::format("SET_RENDER_ENABLE {:#x} (get {:#x})", value, get()));
    if (mode == 1) {
        render_enabled_ = true;
        return;
    }
    if (mode == 2) {
        const auto ea = translate(value & 0xffffff, state_.regs[m::NV4097_SET_CONTEXT_DMA_REPORT], 16);
        if (!ea) {
            fifo_error(std::format("NV4097_SET_RENDER_ENABLE: bad report offset {:#x}", value & 0xffffff));
            return;
        }
        bool pending = backend_.report_pending(*ea + 8);
        if (pending && config_.exact) { // the exact mode waits for the count (D-1029), as the RSX's pipeline would
            backend_.sync();
            for (int i = 0; i < 1000 && backend_.report_pending(*ea + 8); i++) std::this_thread::sleep_for(std::chrono::microseconds(100));
            pending = backend_.report_pending(*ea + 8);
        }
        if (pending) alarms_.add(Alarm::ConditionalPending); // decided on a count not there yet (D-1022)
        render_enabled_ = pending || read32(*ea + 8) != 0;
        return;
    }
    fail(FailKind::Unsupported, reg, std::format("NV4097_SET_RENDER_ENABLE: unknown render mode {}", mode));
}

void Rsx::m_index_array_dma(u32, u32 value)
{
    // RPCS3 nv4097.cpp check_index_array_dma: only location bit 0 and type bit 4 are valid.
    if (value & ~(loc::kMain | (1u << 4))) {
        fifo_error(std::format("invalid NV4097_SET_INDEX_ARRAY_DMA value {:#x}", value));
    }
}

void Rsx::m_dirty_surface(u32, u32) { state_.dirty_bits |= dirty::kSurface | dirty::kPipeline; }
void Rsx::m_dirty_vertex_program(u32, u32) { state_.dirty_bits |= dirty::kVertexProgram; }
void Rsx::m_dirty_fragment_program(u32, u32) { state_.dirty_bits |= dirty::kFragmentProgram; }
// The texture cache invalidated (cellGcmSetInvalidateTextureCache): the fragment program is fetched again, as RPCS3
// does (rsx_methods.cpp binds NV4097_INVALIDATE_L2 to set_shader_program_dirty), and texture reads after it see what
// draws wrote to their targets before it (D-947).
void Rsx::m_invalidate_l2(u32, u32)
{
    state_.dirty_bits |= dirty::kFragmentProgram;
    state_.texture_cache_epoch++;
}
void Rsx::m_dirty_fragment_texture(u32, u32) { state_.dirty_bits |= dirty::kFragmentTextures; }
void Rsx::m_dirty_vertex_texture(u32, u32) { state_.dirty_bits |= dirty::kVertexTextures; }
void Rsx::m_dirty_pipeline(u32, u32) { state_.dirty_bits |= dirty::kPipeline; }
void Rsx::m_dirty_viewport(u32, u32) { state_.dirty_bits |= dirty::kViewport; }
void Rsx::m_dirty_vertex_arrays(u32, u32) { state_.dirty_bits |= dirty::kVertexArrays; }
void Rsx::m_trace(u32 reg, u32 value) { PS_LOG_TRACE(kChan, "RSX method {} arg {:#x}", method_label(reg), value); }

// ---- NV0039 memory to memory ------------------------------------------------------------------------------------

void Rsx::m_nv0039_buffer_notify(u32 reg, u32)
{
    // RPCS3 nv0039.cpp buffer_notify
    const s32 in_pitch = static_cast<s32>(state_.regs[m::NV0039_PITCH_IN]);
    const s32 out_pitch = static_cast<s32>(state_.regs[m::NV0039_PITCH_OUT]);
    const u32 line_length = state_.regs[m::NV0039_LINE_LENGTH_IN];
    const u32 line_count = state_.regs[m::NV0039_LINE_COUNT];
    const u32 in_format = state_.regs[m::NV0039_FORMAT] & 0xff;   // input column stride in bytes
    const u32 out_format = (state_.regs[m::NV0039_FORMAT] >> 8) & 0xff;

    if (!line_length || !line_count) return; // empty transfer is a no-op
    if (in_format == 0 || in_format > 4 || out_format == 0 || out_format > 4) {
        fail(FailKind::Unsupported, reg,
            std::format("NV0039_BUFFER_NOTIFY: invalid column strides {} / {}", in_format, out_format));
        return;
    }
    if (in_pitch < 0 || out_pitch < 0) {
        fail(FailKind::Unsupported, reg, "NV0039_BUFFER_NOTIFY: negative pitch is not implemented");
        return;
    }

    const u32 read_length = static_cast<u32>(in_pitch) * (line_count - 1) + line_length * in_format;
    const u32 write_length = static_cast<u32>(out_pitch) * (line_count - 1) + line_length * out_format;
    const auto src = translate(state_.regs[m::NV0039_OFFSET_IN], state_.regs[m::NV0039_SET_CONTEXT_DMA_BUFFER_IN],
        read_length);
    const auto dst = translate(state_.regs[m::NV0039_OFFSET_OUT],
        state_.regs[m::NV0039_SET_CONTEXT_DMA_BUFFER_OUT], write_length);
    if (!src || !dst) {
        fifo_error(std::format("NV0039_BUFFER_NOTIFY: unmapped transfer (in {:#x}, out {:#x})",
            state_.regs[m::NV0039_OFFSET_IN], state_.regs[m::NV0039_OFFSET_OUT]));
        return;
    }

    const u32 s_ea = *src, d_ea = *dst;
    const auto copy = [this, s_ea, d_ea, read_length, write_length, line_length, line_count, in_pitch, out_pitch,
                          in_format, out_format] {
        u8* const mem = base();
        if (in_format == 1 && out_format == 1) {
            if (static_cast<u32>(in_pitch) == line_length && static_cast<u32>(out_pitch) == line_length) {
                std::memmove(mem + d_ea, mem + s_ea, read_length);
            } else {
                // Overlapping 2D ranges go through a temporary, as RPCS3 does.
                std::vector<u8> tmp(static_cast<size_t>(line_length) * line_count);
                for (u32 y = 0; y < line_count; y++) {
                    std::memcpy(tmp.data() + static_cast<size_t>(y) * line_length, mem + s_ea + y * static_cast<u32>(in_pitch),
                        line_length);
                }
                for (u32 y = 0; y < line_count; y++) {
                    std::memcpy(mem + d_ea + y * static_cast<u32>(out_pitch), tmp.data() + static_cast<size_t>(y) * line_length,
                        line_length);
                }
            }
        } else {
            // Strided byte gather: only the first byte of each column is read and written.
            std::vector<u8> tmp(static_cast<size_t>(line_length) * line_count);
            for (u32 y = 0; y < line_count; y++) {
                for (u32 x = 0; x < line_length; x++) {
                    tmp[static_cast<size_t>(y) * line_length + x] = mem[s_ea + y * static_cast<u32>(in_pitch) + x * in_format];
                }
            }
            for (u32 y = 0; y < line_count; y++) {
                for (u32 x = 0; x < line_length; x++) {
                    mem[d_ea + y * static_cast<u32>(out_pitch) + x * out_format] = tmp[static_cast<size_t>(y) * line_length + x];
                }
            }
        }
        backend_.invalidate_memory(d_ea, write_length);
    };
    if (backend_.tracing())
        backend_.trace_note(std::format("NV0039 copy {:#010x} (pitch {:#x}) -> {:#010x} (pitch {:#x}), {} lines of {} bytes",
            s_ea, in_pitch, d_ea, out_pitch, line_count, line_length));
    // A render target newer on the GPU: copied there (GTA V copies an 80x45 corner of a target into a texture every
    // frame; waiting for the GPU to read it back cost the RSX thread 9 ms a frame, run 079), or else copied once the
    // GPU has it, without waiting (D-107).
    if (in_format == 1 && out_format == 1 && in_pitch > 0 && out_pitch > 0 &&
        backend_.copy_on_gpu(s_ea, static_cast<u32>(in_pitch), d_ea, static_cast<u32>(out_pitch), line_length, line_count)) {
        return;
    }
    if (backend_.flush_memory_later(s_ea, read_length, d_ea, write_length, copy)) return;
    backend_.flush_memory(s_ea, read_length, "an NV0039 buffer copy");
    backend_.flush_memory(d_ea, write_length, "an NV0039 copy over a render target"); // merged with it (D-110)
    copy();
}

// ---- NV308A image from CPU --------------------------------------------------------------------------------------

void Rsx::m_nv308a_color(u32 reg, u32 value)
{
    // RPCS3 nv308a.cpp color::impl: COLOR[i] writes pixel (point.x + i, point.y) of the NV3062 destination.
    const u32 index = reg - m::NV308A_COLOR;
    const u32 out_x_max = state_.regs[m::NV308A_SIZE_OUT] & 0xffff;
    if (index >= out_x_max) return;

    const u32 dst_dma = state_.regs[m::NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN];
    const u32 dst_offset = state_.regs[m::NV3062_SET_OFFSET_DESTIN];
    const u32 out_pitch = state_.regs[m::NV3062_SET_PITCH] >> 16;
    const u32 point = state_.regs[m::NV308A_POINT];
    const u32 x = (point & 0xffff) + index;
    const u32 y = point >> 16;

    switch (state_.regs[m::NV3062_SET_COLOR_FORMAT]) {
    case 10: // A8R8G8B8
    case 11: // Y32
    {
        const auto ea = translate(dst_offset + x * 4 + out_pitch * y, dst_dma, 4);
        if (!ea) {
            fifo_error(std::format("NV308A_COLOR: unmapped destination {:#x}", dst_offset));
            return;
        }
        backend_.before_access(*ea, 4, true);
        backend_.flush_memory(*ea, 4, "an NV308A write over a render target"); // merged with what it renders (D-110)
        write32(*ea, value); // the argument is the pixel in PS3 (big-endian) order
        backend_.invalidate_memory(*ea, 4);
        break;
    }
    case 4: // R5G6B5: the argument is A8R8G8B8 and is converted
    {
        const auto ea = translate(dst_offset + x * 2 + out_pitch * y, dst_dma, 2);
        if (!ea) {
            fifo_error(std::format("NV308A_COLOR: unmapped destination {:#x}", dst_offset));
            return;
        }
        const u32 r = ((value >> 16) & 0xff) * 32 / 255;
        const u32 g = ((value >> 8) & 0xff) * 64 / 255;
        const u32 b = (value & 0xff) * 32 / 255;
        // Stored big-endian, like every 16-bit surface and texture format.
        backend_.before_access(*ea, 2, true);
        backend_.flush_memory(*ea, 2, "an NV308A write over a render target");
        ps_write16(base(), *ea, static_cast<u16>((std::min(r, 31u) << 11) | (std::min(g, 63u) << 5) | std::min(b, 31u)));
        backend_.invalidate_memory(*ea, 2);
        break;
    }
    default:
        fail(FailKind::Unsupported, reg,
            std::format("NV308A_COLOR: NV3062 color format {} not implemented", state_.regs[m::NV3062_SET_COLOR_FORMAT]));
        break;
    }
}

// ---- NV3089 scaled image from memory ----------------------------------------------------------------------------

void Rsx::m_nv3089_image_in(u32 reg, u32 value)
{
    // RPCS3 nv3089.cpp decode_transfer_registers + linear_copy, limited to unscaled copies between equal formats
    // into a pitch (NV3062) surface. Scaling, format conversion and swizzled destinations are reported as
    // unsupported for now; the GPU blit path comes with the surface cache work.
    const auto& r = state_.regs;
    const u32 operation = r[m::NV3089_SET_OPERATION];
    const u32 context_surface = r[m::NV3089_SET_CONTEXT_SURFACE];
    const u32 src_format = r[m::NV3089_SET_COLOR_FORMAT];
    const u32 dst_format = r[m::NV3062_SET_COLOR_FORMAT];
    const u32 out_point = r[m::NV3089_IMAGE_OUT_POINT];
    const u32 out_size = r[m::NV3089_IMAGE_OUT_SIZE];
    const u32 clip_point = r[m::NV3089_CLIP_POINT];
    const u32 clip_size = r[m::NV3089_CLIP_SIZE];
    const u32 in_size = r[m::NV3089_IMAGE_IN_SIZE];
    const u32 in_format = r[m::NV3089_IMAGE_IN_FORMAT];

    const u16 out_x = static_cast<u16>(out_point & 0xffff), out_y = static_cast<u16>(out_point >> 16);
    const u16 out_w = static_cast<u16>(out_size & 0xffff), out_h = static_cast<u16>(out_size >> 16);
    const u16 clip_w = std::min(static_cast<u16>(clip_size & 0xffff), out_w);
    const u16 clip_h = std::min(static_cast<u16>(clip_size >> 16), out_h);
    const u16 clip_x = static_cast<u16>(clip_point & 0xffff), clip_y = static_cast<u16>(clip_point >> 16);
    const u16 in_w = static_cast<u16>(in_size & 0xffff), in_h = static_cast<u16>(in_size >> 16);
    u32 in_pitch = in_format & 0xffff;
    const u32 origin = (in_format >> 16) & 0xff; // 1 center, 2 corner

    if (clip_w == 0 || clip_h == 0) return; // NOP (empty region)
    const bool unscaled = r[m::NV3089_DS_DX] == 0x100000 && r[m::NV3089_DT_DY] == 0x100000;
    const u32 in_bpp = src_format == 7 ? 2 : 4;   // R5G6B5 : A8R8G8B8 / X8R8G8B8
    const u32 out_bpp = dst_format == 4 ? 2 : 4;  // R5G6B5 : A8R8G8B8 / Y32
    const bool formats_match = ((src_format == 3 || src_format == 4) && (dst_format == 10 || dst_format == 11)) ||
        (src_format == 7 && dst_format == 4);
    if (operation != 3 || context_surface != 0x313371C3 || !unscaled || !formats_match || in_w == 0 || in_h == 0) {
        fail(FailKind::Unsupported, reg,
            std::format("NV3089_IMAGE_IN: op {} surface {:#x} fmt {}->{} ds_dx {:#x} dt_dy {:#x} not implemented",
                operation, context_surface, src_format, dst_format, r[m::NV3089_DS_DX], r[m::NV3089_DT_DY]));
        return;
    }

    u32 out_pitch = r[m::NV3062_SET_PITCH] >> 16;
    if (out_pitch == 0) out_pitch = out_bpp * out_w;
    if (in_pitch == 0) in_pitch = in_bpp * in_w;

    // IMAGE_IN: 12.4 fixed point source position (rsx_decode.h NV3089_IMAGE_IN)
    const float fx = static_cast<float>(value & 0xffff) / 16.f;
    const float fy = static_cast<float>(value >> 16) / 16.f;
    u32 in_x, in_y;
    if (origin == 1) {
        in_x = static_cast<u32>(std::max(fx, 0.5f) - 0.5f);
        in_y = static_cast<u32>(std::max(fy, 0.5f) - 0.5f);
    } else {
        in_x = static_cast<u32>(fx);
        in_y = static_cast<u32>(fy);
    }

    const u32 cols = std::min<u32>(clip_w, in_w > in_x ? in_w - in_x : 0);
    const u32 rows = std::min<u32>(clip_h, in_h > in_y ? in_h - in_y : 0);
    if (cols == 0 || rows == 0) return;

    const u32 src_offset = r[m::NV3089_IMAGE_IN_OFFSET] + (in_x + clip_x) * in_bpp + (in_y + clip_y) * in_pitch;
    const u32 dst_offset = r[m::NV3062_SET_OFFSET_DESTIN] + out_x * out_bpp + out_y * out_pitch;
    const u32 read_length = in_pitch * (rows - 1) + cols * in_bpp;
    const u32 write_length = out_pitch * (rows - 1) + cols * out_bpp;
    const auto src = translate(src_offset, r[m::NV3089_SET_CONTEXT_DMA_IMAGE], read_length);
    const auto dst = translate(dst_offset, r[m::NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN], write_length);
    if (!src || !dst) {
        fifo_error(std::format("NV3089_IMAGE_IN: unmapped transfer (src {:#x}, dst {:#x})", src_offset, dst_offset));
        return;
    }

    if (backend_.tracing())
        backend_.trace_note(std::format("NV3089 copy {:#010x} (pitch {:#x}) -> {:#010x} (pitch {:#x}), {}x{}", *src, in_pitch,
            *dst, out_pitch, cols, rows));
    backend_.flush_memory(*src, read_length, "an NV3089 image copy");
    backend_.before_access(*dst, write_length, true);
    backend_.flush_memory(*dst, write_length, "an NV3089 copy over a render target"); // merged with it (D-110)
    u8* const mem = base();
    std::vector<u8> tmp(static_cast<size_t>(cols) * in_bpp * rows);
    for (u32 y = 0; y < rows; y++) std::memcpy(tmp.data() + static_cast<size_t>(y) * cols * in_bpp, mem + *src + y * in_pitch, cols * in_bpp);
    for (u32 y = 0; y < rows; y++) std::memcpy(mem + *dst + y * out_pitch, tmp.data() + static_cast<size_t>(y) * cols * in_bpp, cols * out_bpp);
    backend_.invalidate_memory(*dst, write_length);
}

// ---- flips and lv1 driver methods -------------------------------------------------------------------------------

void Rsx::note_vblank()
{
    {
        std::lock_guard lock(vblank_mutex_);
        vblanks_++;
        last_vblank_ = std::chrono::steady_clock::now();
    }
    vblank_cv_.notify_all();
}

// A flip with vsync waits for the next vblank (rsx.hpp, D-1044; RPCS3 RSXThread.cpp handle_emu_flip with the PS3 frame
// limit: a flip requested at vblank count N happens once the count is past N).
void Rsx::wait_for_vblank()
{
    using Clock = std::chrono::steady_clock;
    std::unique_lock lock(vblank_mutex_);
    const auto now = Clock::now();
    if (last_vblank_ == Clock::time_point{} || now - last_vblank_ > std::chrono::milliseconds(100)) return; // none come
    const u64 at = vblanks_;
    const bool came = vblank_cv_.wait_for(lock, std::chrono::milliseconds(50),
        [&] { return vblanks_ != at || stop_requested_.load(std::memory_order_acquire); });
    flips_held_.fetch_add(1, std::memory_order_relaxed);
    flips_held_ns_.fetch_add(static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - now).count()), std::memory_order_relaxed);
    if (!came) flips_held_timeouts_.fetch_add(1, std::memory_order_relaxed);
}

void Rsx::do_flip(u32 buffer_id)
{
    if (flip_pacing_.load(std::memory_order_relaxed) && vsync_.load(std::memory_order_relaxed)) wait_for_vblank();
    const u64 index = flips_.fetch_add(1, std::memory_order_relaxed) + 1;
    backend_.flip(FlipRequest{buffer_id, display_buffer(buffer_id), index}, state_);
    if (on_flip) on_flip(buffer_id);
}

void Rsx::m_flip_command(u32, u32 value)
{
    // RPCS3 rsx_methods.cpp flip_command (HLE cellGcmSetFlip): the buffer is first queued on head 1, as a
    // GCM_DRIVER_QUEUE queues it (RPCS3 calls the game's queue handler there, before the flip's), then flipped; then
    // the 16-byte flip label (label 1) is cleared.
    m_driver_queue(m::GCM_DRIVER_QUEUE + 1, value);
    do_flip(value);
    ps_write64(base(), config_.layout.label_addr(1), 0);
    ps_write64(base(), config_.layout.label_addr(1) + 8, 0);
}

void Rsx::m_driver_flip(u32 reg, u32 value)
{
    // RPCS3 sys_rsx.cpp sys_rsx_context_attribute 0x102: the high bit selects a queued buffer by index (0xf = the
    // last one queued on this head); otherwise the argument is a display buffer offset.
    const u32 head = reg - m::GCM_FLIP_HEAD;
    u32 id = 0;
    if (value & 0x80000000) {
        id = value & 0xf;
        if (id > 7) id = last_queued_[head].load(std::memory_order_relaxed);
    } else {
        bool found = false;
        for (u32 i = 0; i < 8 && !found; i++) {
            if (const auto b = display_buffer(i); b && b->offset == value) {
                id = i;
                found = true;
            }
        }
        if (!found) {
            fail(FailKind::Unsupported, reg, std::format("GCM_FLIP_HEAD: no display buffer at offset {:#x}", value));
        }
    }
    do_flip(id);
}

void Rsx::m_driver_queue(u32 reg, u32 value)
{
    // RPCS3 sys_rsx.cpp attribute 0x103 (display queue): remember the buffer, let HLE update the driver info.
    const u32 head = reg - m::GCM_DRIVER_QUEUE;
    last_queued_[head].store(value, std::memory_order_relaxed);
    if (on_queue) on_queue(head, value);
}

void Rsx::m_user_command(u32, u32 value)
{
    // RPCS3 rsx_methods.cpp user_command: a full drain, then the user interrupt handler.
    backend_.sync();
    if (on_user_command) on_user_command(value);
}

} // namespace pspocket::rsx
