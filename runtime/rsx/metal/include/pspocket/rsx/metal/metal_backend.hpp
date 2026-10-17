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
//    the start of a pass become load-action clears, and a clear of other targets waits for the next pass that has
//    them (D-945); every attachment is stored (render targets stay on the GPU), unless the next pass clears it whole.
//  - Flip: the display buffer is scaled into an output texture (1152x640 by default, D-016) and handed to the
//    host's present callback; the host does MetalFX and pacing. Chosen flips are also copied to the CPU (capture).

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/rsx/alarms.hpp"
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
// A presented image as a 64x36 thumbnail (D-1027): BGRA8, rows top to bottom (256 bytes each), each texel the mean of
// its box of the image (as pspocket-boot's compare_frame), and the flip it came from. Called on a Metal completion
// thread once the GPU has finished the frame, for every flip; the bytes are only valid during the call.
inline constexpr u32 kThumbnailWidth = 64, kThumbnailHeight = 36;
using ThumbnailCallback = std::function<void(const u8* bgra64x36, u64 flip_index)>;
// A render target at the end of a traced frame, as 8-bit BGRA: name says where it is and its format.
using TraceSurfaceCallback = std::function<void(const std::string& name, const u8* bgra, u32 width, u32 height)>;

// Why render passes end (a pass loads and stores its targets each time), in WindowStats::pass_ends' order (D-148).
// "clear": a clear that waited ran in a pass of its own (D-945).
inline constexpr std::array<const char*, 15> kPassEndNames = {"targets", "gpu copy", "feedback copy", "corner copy",
    "region copy", "grow", "depth conversion", "colour conversion", "target overlay", "submit", "flip", "surface gone",
    "flush", "clear", "other"};

// One report window of the backend (every 300 flips): what its log line says, as numbers, for tools that keep them
// (pspocket-boot --summary, D-148). Per frame unless said otherwise.
struct WindowStats {
    u32 flips = 0;      // in the window
    u64 last_flip = 0;  // the backend's flip count at its end
    double gpu_ms = 0, draws = 0, render_passes = 0;
    std::array<double, kPassEndNames.size()> pass_ends{};
    double readbacks = 0, deferred_readbacks = 0, deferred_conflicts = 0, readback_wait_ms = 0;
    double texture_overlays = 0, texture_checks = 0, texture_mb_hashed = 0, write_faults = 0;
    double feedback_copies = 0, corner_textures = 0, surface_reloads = 0;
    double feedback_reused = 0;         // draws that sampled a feedback copy older than their target's writes (D-947)
    double texture_cache_invalidations = 0; // NV4097_INVALIDATE_L2 commands (D-947)
    double depth_bounds_draws = 0; // draws with the depth bounds test (D-261)
    double attachment_load_mb = 0, attachment_store_mb = 0; // render pass attachments loaded and stored (D-273)
    double attachment_store_skipped_mb = 0; // ... not stored: the next pass clears them whole (D-945)
    double clears_folded = 0;  // clears that became a later pass's load action (D-945)
    double clears_waited = 0;  // clears of other targets that let the pass drawing go on (D-945)
    double gpu_wait_ms = 0;        // the RSX thread's wait for the GPU at flips (frames in flight capped)
    u64 programs_made = 0, pipelines_made = 0; // in the window, by the RSX thread or for it in the background
    double compile_ms = 0;                      // in the window: the RSX thread's compiling, or waiting for compiles
    double draws_compiling = 0;                 // draws skipped while their compile ran in the background (D-158)
    double draws_interpreted = 0;               // draws the shader interpreter drew meanwhile (D-940)
    u64 textures = 0, surfaces = 0;             // cached at its end
    double texture_mb = 0, surface_mb = 0;
    double gpu_allocated_mb = 0;                // MTLDevice.currentAllocatedSize at its end
    u64 deferred_queue = 0;                     // deferred actions waiting at its end
    AlarmCounts alarms{};                       // the correctness alarms raised in the window (D-1022)
};
using WindowCallback = std::function<void(const WindowStats&)>;

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
    // Set: every flip's image downscaled on the GPU to kThumbnailWidth x kThumbnailHeight and handed to it (D-1027):
    // cheap enough for every frame on the phone (a compute pass of 2,304 threads and a 9 KB shared buffer), no wait.
    ThumbnailCallback thumbnail;
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
    // The GPU binaries the cache compiles kept in Metal binary archives in shader_cache, for the next run (D-143): the
    // iPhone app's; macOS keeps them in its own cache, and the Mac's directory is the app's seed.
    bool shader_archives = false;
    // Programs and pipelines a draw needs and nothing has made are compiled on background threads (D-158): the RSX
    // thread waits for them at most compile_wait_ms per frame, then skips the draws still waiting (RPCS3's
    // asynchronous shader mode). Off: each draw waits for its compile, as before (the Mac's comparisons).
    bool async_shaders = false;
    double compile_wait_ms = 4.0;
    u32 async_threads = 0; // 0: half the cores, 2 to 4
    // With async_shaders, a draw whose programs or pipeline are still compiling is drawn by the shader interpreter
    // (D-940: one vertex and one fragment function that run any program) once the interpreter's own pipeline for the
    // draw's targets is made, instead of skipped. force_interpreter (or PSPOCKET_RSX_INTERPRETER=1) sends every draw
    // through it: its results against the translation's (tests).
    bool shader_interpreter = true;
    bool force_interpreter = false;
    // Textures and surfaces unused for this many flips are freed (D-116; a minute at 30 fps).
    u32 evict_after_flips = 1800;
    // A texture of another format over a render target the GPU drew since guest memory last held it is decoded with
    // the target's pixels laid over a copy of the memory on the GPU (D-137). False (or PSPOCKET_RSX_WRITE_BACK_TARGETS
    // set): the target is written back first, a readback the RSX thread waits for, as before.
    bool overlay_targets = true;
    // A new target starts with what guest memory holds (D-1058), as in the exact mode (D-1029): rsx-replay's, whose
    // targets the capture wrote back into memory when it started (a fast run's new target starts undefined: a pooled
    // texture, D-984, or a new one).
    bool load_new_targets = false;
    // Occlusion counters in a command buffer's visibility buffer at first; doubled when one runs out (D-1024).
    u32 occlusion_counters = 4096;
    // Called on the RSX thread with each report window (D-148).
    WindowCallback on_window;
    // What the command buffer being recorded may hold in transient uploads before it is submitted early (D-148): what
    // a frame holds stays allocated until the GPU has run it.
    u64 transient_budget = 256ull << 20;
    // Fewer render passes for a tile GPU (D-945): a clear of targets the open pass does not draw to waits instead of
    // ending it, a clear of a whole target becomes the load action of the next pass that has it, and a pass does not
    // store what the next one clears whole. False (or PSPOCKET_RSX_NO_PASS_MERGING set): every clear ends the pass
    // and runs where it is, as before.
    bool merge_passes = true;
    // The exact mode (D-1029, PSPOCKET_RSX_EXACT=1, a reference for the Mac): what the GPU drew is in guest memory
    // whenever the RSX lets the CPU see anything (every label, semaphore, report and flip waits for the GPU and writes
    // the targets back, on the GPU, into guest memory), a new target starts with what memory holds, copies and
    // readbacks are synchronous, and the fast paths that trade exactness (pass merging, the feedback cache, the
    // surface pool, carrying, D-945, D-947, D-984, D-1025) are off.
    bool exact = false;
};

struct BackendStats {
    u64 draws = 0;
    u64 vertex_bytes_copied = 0; // vertex array bytes copied from guest memory, overlapping attributes once (D-211)
    u64 vertex_overlays = 0;     // targets the GPU drew laid over vertex arrays' copies (render to vertex buffer, D-1034)
    u64 vertex_constant_bytes_copied = 0; // transform constant bytes uploaded, those the programs read (D-278)
    u64 draws_skipped = 0;
    u64 draws_compiling = 0;  // of those, skipped while their program or pipeline compiled in the background (D-158)
    u64 draws_interpreted = 0; // drawn by the shader interpreter while their own compile ran (D-940)
    u64 compile_wait_ns = 0;  // the RSX thread's waits for background compiles, within the frame's budget
    u64 async_programs = 0;   // programs compiled in the background
    u64 async_pipelines = 0;  // ... and pipelines
    u64 async_compile_ns = 0; // the time those took on the background threads
    u64 clears = 0;
    u64 render_passes = 0;
    u64 load_action_clears = 0;
    u64 pipelines_created = 0;
    u64 programs_compiled = 0;
    u64 fragment_programs_decoded = 0;     // fragment programs decoded (each content once, D-240)
    u64 fragment_programs_found_again = 0; // programs at an address that changed found decoded before (D-240)
    u64 pooled_textures_taken = 0; // feedback copies and corners made into a texture an earlier one left (D-956)
    u64 textures_decoded = 0;
    u64 texture_cache_hits = 0;
    // Of the textures decoded, the ones cached before that were decoded again because the target laid over them was
    // drawn to since, or because their memory was written (the rest are new).
    u64 textures_decoded_overlay = 0, textures_decoded_written = 0;
    u64 textures_from_target = 0;   // of the decoded, the ones a target laid over covered whole (D-214)
    u64 texture_bytes_copied = 0;   // bytes copied from guest memory for texture decodes
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
    u64 targets_mirrored = 0;    // targets written into guest memory on the GPU at a flip (PSPOCKET_RSX_MIRROR, D-1030)
    u64 targets_carried = 0;     // new targets that started with the bytes of the GPU-newer ones under them (D-1025)
    // Reads of guest memory under a GPU-newer target (D-1056): the target written back first (a live one), or left
    // (one not drawn or sampled for two flips: its memory taken as reused, as run 132's streamed texture).
    u64 stale_synced = 0, stale_left = 0;
    // Conditional rendering decided on the GPU (D-1058): the predicates set up, and the draw calls made under one.
    u64 gpu_predicates = 0, predicated_draws = 0;
    u64 surface_textures_reused = 0; // surfaces made with a removed surface's texture of their format and size (D-984)
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
    u64 feedback_copies = 0;   // copies of a target a draw samples while drawing to it, made once per write (D-148)
    // ... made for the first such draw since the copy before (D-947), after a write and a texture cache invalidation,
    // after the draws' targets changed, or after a write with the texture cache rule off (PSPOCKET_RSX_STRICT_FEEDBACK)
    u64 feedback_first = 0, feedback_invalidated = 0, feedback_rebound = 0, feedback_strict = 0;
    u64 feedback_reused = 0;   // draws that sampled the copy although the target was written since: the cache's (D-947)
    u64 depth_bounds_draws = 0; // draws with the depth bounds test, which read a copy of the depth buffer (D-261)
    u64 shader_blend_draws = 0; // draws whose fragment shader blended (signed equations, D-948)
    u64 ranges_indexed_by_metal = 0; // indexed ranges Metal ran over the game's own indices (D-271)
    u64 attachment_load_bytes = 0;   // render pass attachments loaded from memory at their start (D-273)
    u64 attachment_store_bytes = 0;  // ... and stored at their end
    u64 clear_only_passes = 0, clear_only_store_bytes = 0; // passes that only cleared (load-action clears, no draw)
    u64 attachment_store_skipped_bytes = 0; // attachments not stored: the next pass clears them whole (D-945)
    u64 clears_folded = 0; // clears that waited and became a later pass's load action (D-945)
    u64 clears_waited = 0; // clears of other targets made while a pass drew, which went on (D-945)
    u64 clear_passes = 0;  // clears that waited and then ran in a render pass of their own (D-945)
    u64 corner_textures = 0;   // corner textures allocated (D-120); later corners of the same size reuse them (D-148)
    u64 gpu_wait_ns = 0;       // the RSX thread's wait at flips for the GPU to finish an older frame (D-148)
    u64 budget_submits = 0;    // command buffers submitted early, past MetalBackendConfig::transient_budget (D-148)
    u64 memory_trims = 0;      // trim_memory() requests served (D-148)
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
    bool report_zpass(u32 value_addr, bool whole) override;
    void draw_skipped(const RsxState& state, std::string_view why) override;
    bool tracing() const override;
    void trace_note(std::string_view what) override;
    bool report_pending(u32 value_addr) override;
    bool predicate_on_gpu(u32 value_addr) override; // D-1058
    void end_predicate() override;

    const BackendStats& stats() const;
    // Any thread: frees what can be made again (textures and surfaces unused for 60 flips, finished readbacks) at the
    // next flip, on the RSX thread. The iOS app calls it when iOS says memory runs low (D-148).
    void trim_memory();
    // Any thread: traces the frame after the next flip as MetalBackendConfig::trace_frame does (the iOS app, when a
    // trace is asked for over USB: D-947).
    void trace_next_frame();
    // The draws drawn so far by fragment program (its content hash), at the last 300-frame report, most first: a run's
    // draw signature, which scripts/rsx_ab.py compares between the fast and the exact modes (D-1029).
    struct DrawSignature {
        u64 flips = 0;
        std::vector<std::pair<u64, u64>> draws;
    };
    DrawSignature draw_signature() const;
    // Waits until the shader cache has made everything it holds (tests), then hands it to the backend.
    void wait_for_shader_cache();
    void wait_for_async_compiles(); // until the background compiles are done (tests; D-158)

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

// EDGE geometry on the GPU (D-988): the Metal source of its kernel ("edge_jobs": pspocket/core/edge_job.h and
// edge_kernels.inc with the Metal meanings of their words), and the compile options it needs (IEEE arithmetic), as an
// MTLCompileOptions* (for the tests, which run it against the same code on the CPU).
std::string edge_kernel_source();
void* edge_kernel_options();

// The EDGE kernel over work items on this machine's GPU, apart from any backend (edge-gpu-check --metal, D-988): each
// job's output (its kept indices, big-endian, then its vertices) and the indirect count; the GPU time of the fastest
// of `repeat` dispatches of them all. Empty outputs: no Metal device or the kernel failed (error says why).
struct EdgeKernelRun {
    std::vector<std::vector<uint8_t>> indices, vertices;
    std::vector<uint32_t> counts;
    double gpu_ms = 0;
    std::string error;
};
EdgeKernelRun edge_run_kernel(const std::vector<std::vector<uint8_t>>& blobs, uint32_t threads, int repeat);

} // namespace pspocket::rsx::metal
