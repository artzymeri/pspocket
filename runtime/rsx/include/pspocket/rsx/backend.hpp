#pragma once

// The interface between the RSX front end (FIFO, methods, state) and a GPU backend (Metal, or a recording backend in
// tests). All calls come from the thread that drives the FIFO (the RSX thread), in command order.

#include <functional>
#include <optional>
#include <string_view>

#include "pspocket/core/types.hpp"
#include "pspocket/rsx/state.hpp"

namespace pspocket::rsx {

class Rsx;

// A display buffer registered by cellGcmSetDisplayBuffer (lv2 sys_rsx_context_attribute 0x104): offset in local
// memory, pitch in bytes, size in pixels. Display buffers are A8R8G8B8 (or X8R8G8B8).
struct DisplayBuffer {
    u32 offset = 0;
    u32 pitch = 0;
    u32 width = 0;
    u32 height = 0;
};

struct FlipRequest {
    u32 buffer_id = 0;
    std::optional<DisplayBuffer> buffer; // nullopt if the id was never registered
    u64 flip_index = 0;                  // count of flips so far, starting at 1
};

class Backend {
public:
    virtual ~Backend() = default;

    // Called once when the Rsx is constructed; gives the backend access to guest memory, the IO map and the
    // failure policy.
    virtual void attach(Rsx& rsx) = 0;

    // NV4097_SET_BEGIN_END(0) with a non-empty draw clause. state.draw describes the draw; the rest of `state` is
    // the render state at the time of the END.
    virtual void draw(const RsxState& state) = 0;

    // NV4097_CLEAR_SURFACE with the clear mask (gcm.hpp clear::*).
    virtual void clear(u32 mask, const RsxState& state) = 0;

    // A flip: present the display buffer. The backend ends the frame and hands the image to the host.
    virtual void flip(const FlipRequest& request, const RsxState& state) = 0;

    // A draw the front end left out (conditional rendering's zero report, D-102), for the backend's frame trace.
    virtual void draw_skipped(const RsxState& state, std::string_view why) { (void)state, (void)why; }
    // The frame trace is on (tracing()): a line about something else the RSX did there (a memory copy).
    virtual bool tracing() const { return false; }
    virtual void trace_note(std::string_view what) { (void)what; }

    // Wait until every command submitted so far has finished on the GPU (NV4097_WAIT_FOR_IDLE, label writes when
    // Config::sync_on_label is set).
    virtual void sync() = 0;

    // The RSX front end wrote guest memory [addr, addr + size) with the CPU (NV0039, NV308A, NV3089 copies): any GPU
    // copy of that memory (cached textures, surfaces) is stale.
    virtual void invalidate_memory(u32 addr, u32 size) = 0;

    // The RSX front end is about to read guest memory [addr, addr + size) with the CPU: data that lives only on the
    // GPU (render targets) must be written back first. Returns true if something was written back (an expensive
    // GPU-to-CPU readback; backends log it).
    virtual bool flush_memory(u32 addr, u32 size, const char* why = "a memory read") = 0;

    // Deferred readbacks (D-107). An RSX copy from [addr, addr + size) to [write_addr, write_addr + write_size)
    // (NV0039) that reads render targets newer on the GPU: the backend writes them back once the GPU has finished them,
    // without waiting, then runs `then` (the copy); returns true. False: the caller copies now. What the RSX writes to
    // guest memory after it (labels, semaphores, notifies, reports) goes through defer(), which runs it at once when
    // nothing is deferred and otherwise queues it behind, so the CPU sees it land after the copy, as it sees the RSX's
    // pipeline reach it. before_access(): the RSX itself reads guest memory there (or samples or renders to a target
    // there), or with `write` writes it: what is deferred and touches it runs first, waiting for the GPU.
    // run_deferred() runs what is ready (wait: all of it). All on the RSX thread.
    // An NV0039 copy of line_count lines of line_length bytes, from rows src_pitch apart to rows dst_pitch apart, done
    // on the GPU when it reads a render target newer there (D-107): the destination becomes a render target holding
    // the copy, which textures over it sample, and guest memory gets it once the GPU has it (as flush_memory_later's
    // readbacks do). True: done, the caller does nothing more; false: the caller copies.
    virtual bool copy_on_gpu(u32 src, u32 src_pitch, u32 dst, u32 dst_pitch, u32 line_length, u32 line_count)
    {
        (void)src, (void)src_pitch, (void)dst, (void)dst_pitch, (void)line_length, (void)line_count;
        return false;
    }
    virtual bool flush_memory_later(u32 addr, u32 size, u32 write_addr, u32 write_size, std::function<void()> then)
    {
        (void)addr, (void)size, (void)write_addr, (void)write_size, (void)then;
        return false;
    }
    virtual void defer(u32 addr, u32 size, std::function<void()> action)
    {
        (void)addr, (void)size;
        action();
    }
    virtual void before_access(u32 addr, u32 size, bool write) { (void)addr, (void)size, (void)write; }
    virtual void run_deferred(bool wait) { (void)wait; }
    virtual bool deferring() const { return false; } // defer() queues now

    // Occlusion queries (D-102): NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE turns counting of the samples that pass the depth
    // and stencil tests on and off, NV4097_CLEAR_REPORT_VALUE(ZPASS_PIXEL_CNT) restarts the count, and
    // NV4097_GET_REPORT(ZPASS_PIXEL_CNT) asks for the count since the last restart in the big-endian word at
    // value_addr: the backend writes it once the GPU has it and returns true, or returns false when it does not count
    // (the front end then writes Config::fake_zpass_count). `whole` (D-1021): the backend writes the whole 16-byte
    // report then, the timestamp at value_addr - 8, the count, and the ready word (pad 0, at value_addr + 4) last, so a
    // CPU that sees it ready sees the count; otherwise the front end writes the timestamp and pad when the method runs.
    virtual void set_zpass_counting(bool on) { (void)on; }
    virtual void clear_zpass() {}
    virtual bool report_zpass(u32 value_addr, bool whole)
    {
        (void)value_addr, (void)whole;
        return false;
    }
    // A ZPASS_PIXEL_CNT report whose count has not reached value_addr yet (the GPU has not finished its draws): what is
    // there is an older count (D-115).
    virtual bool report_pending(u32 value_addr)
    {
        (void)value_addr;
        return false;
    }
    // Conditional rendering on the GPU (D-1058): the draws from now until end_predicate() run only when the pending
    // ZPASS report at value_addr (the last report_zpass for it) counts more than zero, as the GPU decides once it has
    // counted. True when the backend does that; false leaves the decision to the caller (on the count there now).
    virtual bool predicate_on_gpu(u32 value_addr)
    {
        (void)value_addr;
        return false;
    }
    virtual void end_predicate() {}
};

} // namespace pspocket::rsx
