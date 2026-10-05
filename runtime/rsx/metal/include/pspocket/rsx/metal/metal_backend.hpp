#pragma once

// Metal backend for the RSX front end (plan section 7). Objective-C++ inside (metal/src/*.mm, ARC); this header is
// plain C++ so C++ hosts and tests can use it. Metal objects cross the boundary as void* (bridged id<...>).
//
// What it does:
//  - Surface cache: render targets keyed by address/format/pitch/size live on the GPU. Sampling a render target
//    binds its texture; CPU reads of surface memory (flush_memory) are explicit, logged as expensive readbacks.
//  - Texture cache: guest textures decoded on the GPU in compute (ARGB8 and the 16-bit formats, byte swap, Morton
//    unswizzle, borders; DXT1/3/5 copied straight into BC1-3 when the device supports them, decoded in compute
//    otherwise), channel remap applied through texture view swizzles, invalidated through the WriteTracker.
//  - Vertex pulling: vertex shaders read the raw big-endian attribute and index data (shader/msl.hpp).
//  - Pipeline cache keyed by programs, attachment formats, blend and write masks; depth-stencil and sampler caches.
//  - Render passes: one per surface configuration, loading only initialised attachments; full-surface clears at
//    the start of a pass become load-action clears; every attachment is stored (render targets stay on the GPU).
//  - Flip: the display buffer is scaled into an output texture (1152x640 by default, D-016) and handed to the
//    host's present callback; the host does MetalFX and pacing. Chosen flips are also copied to the CPU (capture).

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/rsx/backend.hpp"
#include "pspocket/rsx/state.hpp"
#include "pspocket/rsx/write_tracker.hpp"

namespace pspocket::rsx::metal {

struct PresentFrame {
    // id<MTLTexture>, BGRA8Unorm, output_width x output_height. Owned by the backend's output ring: valid until
    // `frames_in_flight` further presents; retain it (CFBridgingRetain / __bridge) to keep it longer.
    void* texture = nullptr;
    // id<MTLCommandBuffer> that renders the frame. NOT yet committed while the callback runs: the host may add
    // completion handlers; the backend commits it right after the callback returns.
    void* command_buffer = nullptr;
    // id<MTLSharedEvent> signalled to event_value when the frame is complete, for cross-queue waits
    // (encodeWaitForEvent on the host's own queue).
    void* shared_event = nullptr;
    u64 event_value = 0;
    u32 width = 0, height = 0;
    u32 buffer_id = 0;
    u64 flip_index = 0;
};

using PresentCallback = std::function<void(const PresentFrame&)>;
// A presented image on the CPU: BGRA8 bytes, rows top to bottom (width * 4 bytes each), and the flip it came from.
// Called on a Metal completion thread once the GPU has finished the frame; the bytes are only valid during the call.
using CaptureCallback = std::function<void(const u8* bgra, u32 width, u32 height, u64 flip_index)>;
// A render target at the end of a traced frame, as 8-bit BGRA: name says where it is and its format.
using TraceSurfaceCallback = std::function<void(const std::string& name, const u8* bgra, u32 width, u32 height)>;

struct MetalBackendConfig {
    void* device = nullptr;      // id<MTLDevice>; nullptr = MTLCreateSystemDefaultDevice()
    u32 output_width = 1152;     // D-016: native render target, upscaled by the host with MetalFX
    u32 output_height = 640;
    PresentCallback present;
    bool use_bc_textures = true; // DXT -> BC1-3 directly when supported; false forces the compute decoder
    u32 frames_in_flight = 3;
    // Every capture_every-th flip (0: none) is also copied to the CPU and handed to `capture` (debug output: frame
    // dumps on the Mac).
    CaptureCallback capture;
    u32 capture_every = 0;
    // Debug trace of one frame (0: none), the draws between flips trace_frame - 1 and trace_frame: every clear and
    // draw is logged (channel rsx.trace: targets, programs, textures and where they come from, blending), and at its
    // flip every colour target it used goes to trace_surface (float formats clamped to [0, 1]).
    u64 trace_frame = 0;
    TraceSurfaceCallback trace_surface;
    // The shader cache (D-114): programs and pipelines are recorded in shader_cache (a writable directory; empty: not
    // recorded), and the ones recorded there and in shader_seeds (read-only, e.g. the app's copy of the Mac's) are made
    // again on background threads when the backend starts (0 threads: chosen from the core count).
    std::string shader_cache;
    std::vector<std::string> shader_seeds;
    u32 shader_cache_threads = 0;
    // Textures and surfaces unused for this many flips are freed (D-116; a minute at 30 fps).
    u32 evict_after_flips = 1800;
    // A texture of another format over a render target the GPU drew since guest memory last held it is decoded with
    // the target's pixels laid over a copy of the memory on the GPU (D-137). False (or PSPOCKET_RSX_WRITE_BACK_TARGETS
    // set): the target is written back first, a readback the RSX thread waits for, as before.
    bool overlay_targets = true;
};

struct BackendStats {
    u64 draws = 0;
    u64 draws_skipped = 0;
    u64 clears = 0;
    u64 render_passes = 0;
    u64 load_action_clears = 0;
    u64 pipelines_created = 0;
    u64 programs_compiled = 0;
    u64 textures_decoded = 0;
    u64 texture_cache_hits = 0;
    u64 surfaces_created = 0;
    u64 surface_samples = 0; // texture binds served by a render target
    u64 readbacks = 0;       // GPU-to-CPU surface readbacks (expensive)
    u64 readback_wait_ns = 0; // the RSX thread's wait for the GPU before them
    u64 deferred_readbacks = 0; // of the readbacks, those the RSX copied once the GPU had them, without waiting (D-107)
    u64 deferred_conflicts = 0; // RSX accesses that waited for a deferred copy that touches the same memory
    u64 gpu_copies = 0;         // NV0039 copies of render targets done on the GPU (D-107)
    u64 texture_checks = 0;     // cached textures checked against guest memory (a content hash, D-104)
    u64 surface_reloads = 0;    // surfaces loaded again from guest memory the CPU or the RSX wrote
    u64 compile_ns = 0;         // the RSX thread's time in MSL compilation (newLibraryWithSource)
    u64 textures_evicted = 0;    // cached textures freed after a minute unused (D-116)
    u64 surfaces_evicted = 0;    // surfaces freed after a minute unused, their memory holding what they did (D-116)
    u64 surfaces_grown = 0;      // larger targets made over smaller ones at their start, keeping their pixels (D-120)
    u64 surface_corners = 0;     // textures sampled as the top-left corner of a larger target (D-120)
    u64 surface_children = 0;    // targets made inside a larger one, both kept (D-122)
    u64 surface_region_copies = 0; // pixels moved between a target and the larger one it lies in (D-122)
    u64 programs_prewarmed = 0;  // programs the shader cache made ahead and a draw then found (D-114)
    u64 pipelines_prewarmed = 0; // ... and pipelines
    u64 pipeline_ns = 0;        // ... and in render pipeline creation
    u64 surface_guest_changes = 0; // PSPOCKET_RSX_CHECK_SURFACES: samples of surfaces whose memory changed under them
    u64 texture_check_bytes = 0; // the bytes those checks hashed
    u64 depth_conversions = 0; // depth buffers converted on the GPU for a texture that reads them (D-088)
    u64 color_conversions = 0; // colour targets converted on the GPU for a texture of another format (D-098)
    u64 texture_overlays = 0;  // targets laid over guest memory on the GPU for a texture decoded from it (D-137)
    u64 occlusion_reports = 0; // NV4097_GET_REPORT(ZPASS_PIXEL_CNT) answered from the GPU's counters (D-102)
    u64 flips = 0;
    u64 command_buffers = 0;
};

class MetalBackend final : public Backend {
public:
    MetalBackend(GuestMemory& mem, WriteTracker& tracker, MetalBackendConfig config = {});
    ~MetalBackend() override;
    MetalBackend(const MetalBackend&) = delete;
    MetalBackend& operator=(const MetalBackend&) = delete;

    // False when no Metal device exists (the backend then drops everything with a log line).
    bool valid() const;
    void* device() const; // id<MTLDevice>
    bool supports_bc() const;

    // Backend
    void attach(Rsx& rsx) override;
    void draw(const RsxState& state) override;
    void clear(u32 mask, const RsxState& state) override;
    void flip(const FlipRequest& request, const RsxState& state) override;
    void sync() override;
    void invalidate_memory(u32 addr, u32 size) override;
    bool flush_memory(u32 addr, u32 size, const char* why = "a memory read") override;
    bool copy_on_gpu(u32 src, u32 src_pitch, u32 dst, u32 dst_pitch, u32 line_length, u32 line_count) override;
    bool flush_memory_later(u32 addr, u32 size, u32 write_addr, u32 write_size, std::function<void()> then) override;
    void defer(u32 addr, u32 size, std::function<void()> action) override;
    void before_access(u32 addr, u32 size, bool write) override;
    bool deferring() const override;
    void run_deferred(bool wait) override;
    void set_zpass_counting(bool on) override;
    void clear_zpass() override;
    bool report_zpass(u32 value_addr) override;
    bool report_pending(u32 value_addr) override;

    const BackendStats& stats() const;
    // Waits until the shader cache has made everything it holds (tests), then hands it to the backend.
    void wait_for_shader_cache();

    // Debug and test helpers ------------------------------------------------------------------------------------
    // Decodes texture `t` (its bytes at guest address `ea`) through the texture path and reads back (face, level)
    // as RGBA floats, row-major. force_compute_dxt decodes DXT in compute even when BC is supported.
    bool debug_decode_texture(const TextureInfo& t, u32 ea, u32 face, u32 level, bool force_compute_dxt,
        std::vector<std::array<float, 4>>& out, std::string* error = nullptr);
    // Reads a color surface (by guest address) as RGBA8 bytes (r, g, b, a per pixel), after finishing pending work.
    bool debug_read_surface(u32 ea, std::vector<u8>& rgba, u32& width, u32& height);
    // Last error from shader translation or compilation (for tests and logs).
    const std::string& last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pspocket::rsx::metal
