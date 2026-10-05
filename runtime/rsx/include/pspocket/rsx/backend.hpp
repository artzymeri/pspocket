#pragma once

// The interface between the RSX front end (FIFO, methods, state) and a GPU backend (Metal, or a recording backend in
// tests). All calls come from the thread that drives the FIFO (the RSX thread), in command order.

#include <optional>

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

    // Occlusion queries (D-102): NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE turns counting of the samples that pass the depth
    // and stencil tests on and off, NV4097_CLEAR_REPORT_VALUE(ZPASS_PIXEL_CNT) restarts the count, and
    // NV4097_GET_REPORT(ZPASS_PIXEL_CNT) asks for the count since the last restart in the big-endian word at
    // value_addr: the backend writes it once the GPU has it and returns true, or returns false when it does not count
    // (the front end then writes Config::fake_zpass_count).
    virtual void set_zpass_counting(bool on) { (void)on; }
    virtual void clear_zpass() {}
    virtual bool report_zpass(u32 value_addr)
    {
        (void)value_addr;
        return false;
    }
};

} // namespace pspocket::rsx
