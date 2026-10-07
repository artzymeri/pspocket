#pragma once

// Internal Objective-C++ state of the Metal backend, shared by the .mm files in metal/src. Not a public header.

#if !__has_feature(objc_arc)
#error "the Metal backend is compiled with ARC (-fobjc-arc / CLANG_ENABLE_OBJC_ARC)"
#endif

#import <Metal/Metal.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/rsx/metal/metal_backend.hpp"
#include "pspocket/rsx/rsx.hpp"
#include "pspocket/rsx/shader/ir.hpp"
#include "pspocket/rsx/shader/msl.hpp"
#include "pspocket/rsx/texture.hpp"
#include "pspocket/rsx/write_tracker.hpp"

namespace pspocket::rsx::metal {

inline constexpr std::string_view kChan = "rsx.metal";

// ---- transient memory ---------------------------------------------------------------------------------------------

// Per-command-buffer bump allocator over shared 4 MB chunks; chunks return to the pool when the command buffer that
// used them completes.
class TransientAllocator {
public:
    struct Allocation {
        id<MTLBuffer> buffer = nil;
        u32 offset = 0;
        u8* data = nullptr;
    };

    void init(id<MTLDevice> device) { device_ = device; }
    Allocation alloc(u32 size, u32 align = 256);
    // Hands every chunk used since the last call to `cb`; they come back when it completes.
    void retire(id<MTLCommandBuffer> cb);
    // Changes at each retire: an allocation made in this generation is still valid to bind.
    u64 generation() const { return generation_; }
    // What the command buffer being recorded holds (chunks and buffers of its own) until it is retired (D-148).
    u64 bytes_in_use() const { return bytes_in_use_; }

private:
    static constexpr u32 kChunkSize = 4u << 20;
    u64 generation_ = 0;
    u64 bytes_in_use_ = 0;
    id<MTLDevice> device_ = nil;
    id<MTLBuffer> current_ = nil;
    u32 offset_ = 0;
    std::vector<id<MTLBuffer>> in_use_;
    std::shared_ptr<std::mutex> free_mutex_ = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<id<MTLBuffer>>> free_ = std::make_shared<std::vector<id<MTLBuffer>>>();
};

// ---- surfaces -----------------------------------------------------------------------------------------------------

// rsx_depth_to_texture's mode for the depth texture formats: the depth itself (kernels.hpp kDepthValue).
inline constexpr u32 kDepthValueMode = 100;

struct Surface {
    u32 address = 0;   // guest EA
    u32 pitch = 0;
    u32 width = 0, height = 0;
    u32 gcm_format = 0; // SurfaceColorFormat or SurfaceDepthFormat
    bool depth = false;
    bool swizzled = false;
    MTLPixelFormat pixel_format = MTLPixelFormatInvalid;
    id<MTLTexture> texture = nil;
    bool initialized = false;  // has content (rendered or cleared): load instead of clear at the next pass
    bool gpu_newer = false;    // rendered since guest memory was last written back
    bool cpu_newer = false;    // guest memory was written by the CPU after the GPU copy was made
    u64 last_used = 0;
    u64 last_frame = 0;        // the flip it was last rendered to or sampled in (D-116: unused a minute, freed)
    std::unordered_map<u32, id<MTLTexture>> views; // swizzled views for sampling, by encoded channel sources
    u64 generation = 0;         // writes to it: colour, or depth for a depth buffer (D-105)
    u64 stencil_generation = 0; // writes to a depth buffer's stencil
    u64 guest_hash = 0;          // PSPOCKET_RSX_CHECK_SURFACES: its guest memory when it last matched or was made
    u64 serial = 0;              // unique among the backend's surfaces, never reused (D-137)
    // A depth buffer (D-088) or colour target (D-098) read as a texture of another format: its conversion per decode
    // mode, the generation it was made from, and its swizzled views.
    struct Conversion {
        id<MTLTexture> texture = nil;
        u64 generation = 0;
        u64 stencil_generation = 0;
        std::unordered_map<u32, id<MTLTexture>> views;
    };
    std::unordered_map<u32, Conversion> conversions;
    // Its top-left corner sampled as a smaller texture at its start (D-120), by width << 16 | height.
    std::unordered_map<u32, Conversion> corners;
    // A copy for draws that sample it while they draw to it (D-148): made again only once it was written since. A copy
    // per draw was up to ~200 whole targets a frame, held until the GPU ran the frame (4.7 GB on the iPhone, run 089).
    Conversion feedback;
    // A colour target inside a larger one with its pitch and format, not at its start (D-122): its own texture, the
    // larger one its parent. What one writes reaches the other on the GPU before the other is used: a child's pixels
    // go into its place in the parent; after the parent is written, the child takes its place back from it.
    Surface* parent = nullptr;
    u32 parent_x = 0, parent_y = 0;
    std::vector<Surface*> children;
    bool newer_than_parent = false;
    bool older_than_parent = false;
    // A copy's destination (D-107), once guest memory has the copy: a CPU write after that makes guest memory newer.
    WriteTracker::Watch watch{};
    u32 bytes() const { return pitch * height; }
};

// ---- textures -----------------------------------------------------------------------------------------------------

struct CachedTexture {
    u32 address = 0;
    TextureInfo info{};
    TextureLayout layout{};
    id<MTLTexture> texture = nil;
    WriteTracker::Watch watch{};
    std::unordered_map<u32, id<MTLTexture>> views; // by encoded channel sources
    u64 last_used = 0;
    u64 last_frame = 0;  // the flip it was last sampled in (D-116)
    u64 checked = ~0ull; // the check epoch (MetalBackend::Impl::check_epoch) it was last found clean in (D-104)
    // The GPU-newer target laid over guest memory when it was decoded (D-137; serial 0: none): the texture is out of
    // date once that target is drawn to again, or another one (or none) is newer than memory there.
    struct Overlay {
        u64 serial = 0, generation = 0, stencil_generation = 0;
        bool operator==(const Overlay&) const = default;
    } overlay;
};

// ---- pipelines ----------------------------------------------------------------------------------------------------

struct CompiledProgram {
    id<MTLFunction> function = nil;
    shader::MslShader shader;
    std::vector<u32> ucode; // instruction words (FP: constants masked) for collision checks
    std::string key;        // its key in MetalBackend::Impl::programs (the shader cache records it by it, D-114)
};

// What a render pipeline is made from besides its two programs: the attachments, their write masks and blending
// (RSX blending is one function for every target, enabled per target). pipeline_for and the shader cache (D-114)
// make the descriptor and the key from it alike.
struct PipelineParams {
    u32 color_count = 0;
    std::array<u32, 4> color_formats{}; // MTLPixelFormat
    std::array<u32, 4> write_masks{};   // MTLColorWriteMask
    std::array<u32, 4> blend_on{};
    u32 sfactor_rgb = 0, dfactor_rgb = 0, sfactor_a = 0, dfactor_a = 0, equation_rgb = 0, equation_a = 0;
    u32 depth_format = 0, stencil_format = 0;
};


struct PipelineKey {
    u64 vp = 0, fp = 0;
    std::array<u32, 4> color_formats{};
    u32 depth_format = 0;
    u32 stencil_format = 0;
    std::array<u32, 4> write_masks{};
    std::array<u32, 4> blend{}; // enable | factors | ops packed per target (see pipeline_cache.mm)
    std::array<u32, 4> blend_ops{};
    bool operator<(const PipelineKey& o) const;
};

MTLCompileOptions* shader_compile_options(); // pipeline_cache.mm: what every program compiles with
PipelineKey pipeline_key(const PipelineParams& p, id<MTLFunction> vp, id<MTLFunction> fp);
MTLRenderPipelineDescriptor* pipeline_descriptor(const PipelineParams& p, id<MTLFunction> vp, id<MTLFunction> fp);

// The shader cache (shader_cache.mm, D-114): records what the backend compiles, and makes what was recorded before on
// background threads; the RSX thread takes the results between draws (MetalBackend::Impl::take_shader_cache).
class ShaderCache {
public:
    struct Record {
        std::string key;
        shader::MslShader shader;
        std::vector<u32> ucode;
    };
    struct PipelineRecord {
        std::string vp_key, fp_key;
        PipelineParams params;
    };
    struct ReadyPipeline {
        std::string vp_key, fp_key;
        PipelineParams params;
        __strong id<MTLFunction> vp = nil, fp = nil;
        __strong id<MTLRenderPipelineState> pso = nil;
    };
    struct Ready {
        std::vector<CompiledProgram> programs;
        std::vector<ReadyPipeline> pipelines;
    };

    ShaderCache(id<MTLDevice> device, std::string dir, std::vector<std::string> seeds, u32 threads, bool archives);
    ~ShaderCache();
    ShaderCache(const ShaderCache&) = delete;
    ShaderCache& operator=(const ShaderCache&) = delete;

    bool take(Ready& out); // what was made since the last call; false: nothing
    void wait();           // until every record has been made (or failed)
    void record_program(const CompiledProgram& p);
    void record_pipeline(const std::string& vp_key, const std::string& fp_key, const PipelineParams& p);
    // The GPU binaries earlier runs kept (D-143), for the RSX thread's own pipelines too; nil until loaded.
    NSArray<id<MTLBinaryArchive>>* archives();

private:
    void load_and_start(u32 threads);
    void work(u32 worker);
    id<MTLFunction> program(const std::string& key, MTLCompileOptions* options);
    void load_archives();
    void report_progress();

    __strong id<MTLDevice> device_ = nil;
    std::string dir_;
    std::vector<std::string> seeds_;
    bool use_archives_ = false;
    std::chrono::steady_clock::time_point started_;
    std::thread loader_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{false}, ready_{false};
    std::mutex mutex_;
    std::condition_variable done_cv_;
    bool done_ = false;
    std::vector<Record> records_;
    std::vector<PipelineRecord> pipeline_records_;
    std::unordered_set<std::string> recorded_programs_, recorded_pipelines_;
    std::unordered_map<std::string, id<MTLFunction>> functions_;
    std::unordered_map<std::string, usize> record_index_; // program key -> records_
    std::unordered_set<std::string> compiling_;            // programs a worker is compiling now
    std::condition_variable compiled_cv_;
    std::vector<CompiledProgram> ready_programs_;
    std::vector<ReadyPipeline> ready_pipelines_;
    u64 made_programs_ = 0, made_pipelines_ = 0, archive_hits_ = 0, archived_ = 0;
    std::atomic<usize> next_program_{0}, next_pipeline_{0};
    u32 workers_left_ = 0;
    __strong NSArray<id<MTLBinaryArchive>>* archives_ = nil; // read only once loaded
    std::chrono::steady_clock::time_point last_report_;
};

// Asynchronous compilation (async_compiler.mm, D-158): a program or pipeline a draw needs and the backend lacks is
// compiled on background threads. The RSX thread waits for it only within a small budget per frame, then draws on
// without it (the draw is skipped, as RPCS3's asynchronous shader mode does), and takes what finished between draws
// (MetalBackend::Impl::take_async). For any game, the first time anything new shows, nothing waits for the Metal compiler
// longer than the budget.
class AsyncCompiler {
public:
    struct ProgramJob {
        std::string key;
        shader::MslShader shader;
        std::vector<u32> ucode;
        const char* what = "";
    };
    struct PipelineJob {
        PipelineKey key;
        __strong MTLRenderPipelineDescriptor* descriptor = nil;
        std::string vp_key, fp_key;
        PipelineParams params;
    };
    struct ReadyPipeline {
        PipelineKey key;
        __strong id<MTLRenderPipelineState> pso = nil;
        std::string vp_key, fp_key;
        PipelineParams params;
        u64 ns = 0;
    };
    struct Ready {
        std::vector<std::pair<CompiledProgram, u64>> programs; // with the time it took
        std::vector<ReadyPipeline> pipelines;
        std::vector<std::pair<std::string, std::string>> failed_programs; // key, error (with the source)
        std::vector<std::pair<PipelineKey, std::string>> failed_pipelines;
    };

    AsyncCompiler(id<MTLDevice> device, u32 threads);
    ~AsyncCompiler();
    AsyncCompiler(const AsyncCompiler&) = delete;
    AsyncCompiler& operator=(const AsyncCompiler&) = delete;

    bool queued(const std::string& program_key);
    bool queued(const PipelineKey& key);
    void queue(ProgramJob job);
    void queue(PipelineJob job);
    // Until something more is ready to take, or `deadline`. True when something is.
    bool wait_until(std::chrono::steady_clock::time_point deadline);
    void wait_idle(); // until nothing is queued or compiling (tests)
    bool take(Ready& out);

private:
    void work();

    __strong id<MTLDevice> device_ = nil;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable work_cv_, ready_cv_;
    bool stop_ = false;
    u32 busy_ = 0;
    std::deque<ProgramJob> programs_;
    std::deque<PipelineJob> pipelines_;
    std::unordered_set<std::string> program_keys_; // queued or compiling
    std::map<PipelineKey, bool> pipeline_keys_;
    Ready ready_;
    std::atomic<bool> has_ready_{false};
};

struct CompiledProgram;

struct FragmentProgramEntry {
    u32 address = 0;
    shader::FragmentProgram program;
    WriteTracker::Watch watch{};
    u64 checked = ~0ull; // as CachedTexture::checked
    // What the program compiled to with these options, until it is decoded again (D-109): the lookup by hash and
    // ucode is not repeated every draw.
    std::vector<std::pair<shader::FpOptions, const CompiledProgram*>> compiled;
    // The program's bytes as decoded, its embedded constants zeroed: written again with the same instructions (only
    // new constants, or the same program uploaded again), it is not decoded again (D-210).
    std::vector<u8> code;
};

// ---- the backend --------------------------------------------------------------------------------------------------

struct MetalBackend::Impl {
    Impl(GuestMemory& m, WriteTracker& t, MetalBackendConfig c) : mem(m), tracker(t), config(std::move(c)) {}

    GuestMemory& mem;
    WriteTracker& tracker;
    MetalBackendConfig config;
    Rsx* rsx = nullptr;
    BackendStats stats{};
    std::string last_error;
    u64 tick = 0;
    // The frame trace (MetalBackendConfig::trace_frame): on while its draws run; the colour targets they used.
    bool tracing = false;
    u32 trace_count = 0;
    std::vector<u32> trace_targets;
    // The 2D textures the traced frame sampled, each once, as the shader sees them (its channel order).
    struct TracedTexture {
        u32 unit, address, format;
        id<MTLTexture> texture;
    };
    std::vector<TracedTexture> trace_textures;
    const char* bind_source = ""; // how bind_texture served the last texture (for the trace)

    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLLibrary> kernels = nil;
    id<MTLComputePipelineState> decode_pipeline = nil;
    id<MTLComputePipelineState> read_pipeline = nil;
    id<MTLComputePipelineState> depth_pipeline = nil; // rsx_depth_to_texture
    id<MTLComputePipelineState> color_pipeline = nil; // rsx_color_to_texture
    id<MTLComputePipelineState> decode3d_pipeline = nil; // rsx_decode_texture3d
    // D-137: a target's pixels as guest bytes into a texture's staging copy of memory (rsx_*_to_memory).
    id<MTLComputePipelineState> color_memory_pipeline = nil, float_memory_pipeline = nil, depth_memory_pipeline = nil;
    u64 surface_serial = 0; // the last Surface::serial given
    id<MTLTexture> dummy_stencil = nil;              // for Z16 buffers, which have none
    id<MTLRenderPipelineState> blit_pipeline = nil;
    id<MTLSamplerState> nearest_sampler = nil;
    id<MTLSamplerState> linear_sampler = nil;
    id<MTLTexture> dummy_texture = nil;
    id<MTLTexture> dummy_cube = nil;
    id<MTLTexture> dummy_volume = nil; // 3D
    id<MTLTexture> dummy_depth = nil;  // Depth32Float, for depth2d units
    id<MTLSharedEvent> frame_event = nil;
    u64 frame_event_value = 0;
    bool bc_supported = false;
    TransientAllocator transient;
    TransientAllocator::Allocation consts_upload{}; // the transform constants as last uploaded (D-129)
    u64 consts_generation = ~0ull;                   // the transient generation they were uploaded in

    // Command buffers: uploads (texture decode) go to their own buffer, committed before the render buffer.
    id<MTLCommandBuffer> upload_cb = nil;
    id<MTLCommandBuffer> render_cb = nil;
    id<MTLCommandBuffer> last_committed = nil;

    // Render pass
    struct PassTargets {
        std::array<Surface*, 4> color{};
        std::array<u32, 4> rsx_index{}; // RSX surface (A=0 .. D=3) behind each attachment: blend and mask bits
        u32 color_count = 0;
        Surface* depth = nullptr;
        // The surface clip: what the pass draws in, the top-left corner of its surfaces, which can be larger (a target
        // drawn narrower than the one at its address before it, D-120).
        u32 width = 0, height = 0;
        bool operator==(const PassTargets& o) const
        {
            return color == o.color && color_count == o.color_count && depth == o.depth && width == o.width &&
                height == o.height;
        }
        bool contains(const Surface* s) const
        {
            for (u32 i = 0; i < color_count; i++) {
                if (color[i] == s) return true;
            }
            return depth == s;
        }
    };
    MTLRenderPassDescriptor* pass_desc = nil;
    id<MTLRenderCommandEncoder> encoder = nil;

    // Occlusion queries (D-102): each render buffer has a visibility buffer of 64-bit counters; a query counts into one
    // counter per render pass it draws in (Metal writes a pass's counters when the pass ends), and a report sums them
    // once the render buffer that holds the last of them has completed.
    struct VisibilitySlot {
        __strong id<MTLBuffer> buffer;
        u32 index = 0;
    };
    struct PendingReport {
        u32 value_addr = 0;
        std::vector<VisibilitySlot> slots;
    };
    static constexpr u32 kVisibilitySlots = 4096;

    // GPU time of the render buffers (completion handlers add GPUEndTime - GPUStartTime), logged with the frame's draws
    // and passes every kGpuReportFlips flips: what a slower GPU (the iPhone's) has to fit in a frame.
    static constexpr u64 kGpuReportFlips = 300;
    std::shared_ptr<std::atomic<u64>> gpu_ns = std::make_shared<std::atomic<u64>>(0);
    // Why render passes ended (a started pass only), for the same report: the pass loads and stores its targets. The
    // copies between targets are told apart (D-148): an NV0039 copy on the GPU, a target sampled while drawn to, a
    // corner of a larger target, pixels between a target and its parent, a target grown over a smaller one.
    enum PassEnd : u8 { kEndTargets, kEndGpuCopy, kEndFeedbackCopy, kEndCornerCopy, kEndRegionCopy, kEndGrow,
                        kEndDepthConversion, kEndColorConversion, kEndTargetOverlay, kEndSubmit, kEndFlip, kEndSurfaceGone,
                        kEndFlush, kEndOther, kEndCount };
    static_assert(kEndCount == kPassEndNames.size(), "kPassEndNames names every PassEnd");
    PassEnd pass_end_reason = kEndOther;
    std::array<u64, kEndCount> pass_ends{}, report_pass_ends{};
    void end_pass_for(PassEnd why)
    {
        pass_end_reason = why;
        end_pass();
        pass_end_reason = kEndOther;
    }
    u64 report_flips = 0, report_draws = 0, report_passes = 0, report_gpu_ns = 0, report_readback_ns = 0;
    u64 report_readbacks = 0, report_deferred = 0, report_conflicts = 0, report_texture_checks = 0, report_texture_bytes = 0;
    u64 report_faults = 0; // the tracker's write faults at the last report (D-134)
    u64 report_overlays = 0;
    u64 report_programs = 0, report_pipelines = 0, report_compile_ns = 0, report_draws_compiling = 0;
    u64 report_feedback = 0, report_corners = 0, report_reloads = 0, report_gpu_wait_ns = 0;
    // At most config.frames_in_flight frames queued for the GPU (D-148): a flip waits for the oldest to complete. Each
    // frame queued holds what it allocated until then, and nothing else held the RSX thread back.
    dispatch_semaphore_t frames_in_flight = nil;
    u32 frames_in_flight_limit = 0;
    std::atomic<bool> trim_requested{false}; // MetalBackend::trim_memory
    void trim();                             // serves it, at a flip
    __strong id<MTLBuffer> visibility = nil; // the render buffer's counters
    u32 visibility_next = 0;
    bool zpass_on = false;
    std::vector<VisibilitySlot> zpass_slots; // the counters of the query since the last clear
    s32 pass_zpass_slot = -1;                // this pass's counter for that query, -1 until a draw counts
    s32 encoder_zpass_slot = -1;             // the counter the encoder counts into, -1 when it does not count
    std::vector<PendingReport> pending_reports;
    // Reports registered and not written yet, by value address (D-115): the completion handlers count them down.
    struct ReportsInFlight {
        std::mutex mutex;
        std::unordered_map<u32, u32> count;
    };
    std::shared_ptr<ReportsInFlight> reports_in_flight = std::make_shared<ReportsInFlight>();
    PassTargets pass{};
    PassTargets draw_targets{}; // targets of the draw being prepared (render-target feedback detection)
    bool pass_open = false;    // targets chosen (descriptor built)
    bool pass_partial = false; // the pass draws in part of an attachment: its load-action clear would clear the rest
    bool pass_started = false; // encoder created
    u32 pass_width = 0, pass_height = 0;

    // The texture and program caches check guest memory (a content hash) at most once per check epoch, which advances
    // with the RSX's data epoch and with the writes the RSX and this backend make to guest memory themselves (D-104).
    u64 check_counter = 1;
    u64 seen_data_epoch = 0;
    u64 check_epoch()
    {
        const u64 d = rsx ? rsx->data_epoch() : 0;
        if (d != seen_data_epoch) {
            seen_data_epoch = d;
            check_counter++;
        }
        return check_counter;
    }
    void own_write() { check_counter++; }

    // Caches
    std::vector<std::unique_ptr<Surface>> surfaces;
    std::unordered_map<u64, std::unique_ptr<CachedTexture>> textures; // key: hash of decode inputs
    std::unordered_set<u64> reinterpretations; // (target format, texture format, width) pairs logged once
    std::map<PipelineKey, id<MTLRenderPipelineState>> pipelines;
    std::unordered_map<std::string, CompiledProgram> programs; // by program hash and translation options
    std::unique_ptr<ShaderCache> shader_cache;                 // D-114; null: none configured
    std::unique_ptr<AsyncCompiler> async;                      // D-158; null: draws wait for their compiles
    std::unordered_set<std::string> async_failed_programs;     // compiled in the background and failed: not tried again
    std::map<PipelineKey, bool> async_failed_pipelines;
    bool compile_pending = false;     // the last lookup's program or pipeline is compiling in the background
    s64 compile_wait_left_ns = 0;     // what this frame may still wait for background compiles
    void take_async();
    // Waits (within the frame's budget) for `done` to hold, taking what finishes meanwhile.
    bool wait_async(const std::function<bool()>& done);
    u64 texture_bytes() const; // what the cached textures take in guest memory (their decoded copies, about as much)
    u64 surface_bytes() const; // what the surfaces' GPU copies take
    // Textures unused for a minute of flips are freed (decoded again if used again), and surfaces unused as long whose
    // guest memory holds what they do (D-116): GTA V streams textures to new places all the time, and a phone has a few
    // GB, not a Mac's.
    void evict_unused(u64 unused_flips);
    void take_shader_cache();
    std::unordered_map<u64, id<MTLDepthStencilState>> depth_states;
    std::unordered_map<u64, id<MTLSamplerState>> samplers;
    std::unordered_map<u64, id<MTLRenderPipelineState>> clear_pipelines;
    std::unordered_map<u32, id<MTLRenderPipelineState>> blit_pipelines; // by destination MTLPixelFormat
    std::unordered_map<u32, FragmentProgramEntry> fragment_programs; // by guest EA
    // Vertex programs decoded lately, the current one first (D-210). A game that uploads its programs again before
    // nearly every draw (Assassin's Creed's crowds: 2,150 draws a frame) finds the one it uploads here by its words
    // instead of decoding it again: decoding was 7% of the RSX thread.
    struct DecodedVertexProgram {
        shader::VertexProgram program;
        std::vector<u32> words;                         // its instructions' words, from its start to its end
        std::array<const CompiledProgram*, 2> compiled{}; // it compiled, without and with point size (D-109)
    };
    static constexpr usize kDecodedVertexPrograms = 16;
    std::vector<DecodedVertexProgram> vertex_programs;
    bool vertex_program_ok = false; // vertex_programs.front() is the program the state holds
    void forget_program_lookups();                                   // a program key now names another program
    std::vector<id<MTLTexture>> outputs;
    u32 next_output = 0;

    // ---- core (metal_backend.mm) ----
    bool init();
    id<MTLCommandBuffer> render_buffer();
    id<MTLCommandBuffer> upload_buffer();
    void submit(bool wait);
    void fail(std::string message);
    void unsupported(std::string message);

    // Render passes
    // clear_mask: for a clear, its mask (only a clear that touches depth or stencil looks at the depth buffer);
    // ~0u for a draw.
    bool select_targets(const RsxState& state, PassTargets& out, u32 clear_mask);
    void open_pass(const PassTargets& targets);
    id<MTLRenderCommandEncoder> start_encoder();
    void end_pass();

    void do_draw(const RsxState& state);
    void do_clear(u32 mask, const RsxState& state);
    void do_flip(const FlipRequest& request);
    void trace_targets_of(const PassTargets& t);
    static std::string describe_targets(const PassTargets& t);
    void trace_dump();

    // ---- surfaces (surface_cache.mm) ----
    Surface* get_color_surface(u32 address, u32 pitch, u32 width, u32 height, u32 format, bool swizzled);
    Surface* get_depth_surface(u32 address, u32 pitch, u32 width, u32 height, u32 format, bool depth_float);
    Surface* find_surface(u32 address, bool depth) const;
    Surface* make_surface(u32 address, u32 pitch, u32 width, u32 height, u32 format, bool depth, bool swizzled,
        MTLPixelFormat pf);
    void evict_overlapping(u32 address, u32 pitch, u32 row_bytes, u32 rows, const Surface* keep,
        const Surface* keep_too = nullptr);
    // D-122: the larger target a new one lies inside; the pixels between them before one is used; a write to one.
    Surface* find_parent(u32 address, u32 pitch, u32 width, u32 height, u32 format) const;
    void sync_surface(Surface& s);
    void surface_written(Surface& s);
    void copy_region(id<MTLTexture> src, u32 sx, u32 sy, id<MTLTexture> dst, u32 dx, u32 dy, u32 w, u32 h);
    bool read_back(Surface& s, std::vector<u8>& raw, bool stencil = false); // raw texture bytes, tightly packed
    bool write_back(Surface& s, const char* why);     // to guest memory, in the surface's format
    bool write_back_depth(Surface& s, const char* why);

    // Deferred readbacks (D-107): a surface's texels blitted into staging buffers in a render buffer committed at once,
    // written to guest memory once it has completed, then an action of the RSX's; later actions queue behind them.
    struct SurfaceImage { // what a write-back needs of a surface (the surface itself may be gone by then)
        u32 address = 0, pitch = 0, width = 0, height = 0, gcm_format = 0;
        bool depth = false;
        bool swizzled = false; // texels in Morton order (D-111)
        MTLPixelFormat pixel_format = MTLPixelFormatInvalid;
    };
    struct Readback {
        SurfaceImage image;
        __strong id<MTLBuffer> data = nil;    // the texels, rows packed
        __strong id<MTLBuffer> stencil = nil; // Z24S8: the stencil, rows packed
        // A GPU copy's destination (copy_on_gpu): unless written again since (generation), it is no longer newer than
        // guest memory once this lands, and its memory is watched for the CPU's writes.
        __strong id<MTLTexture> mirror = nil;
        u64 generation = 0;
    };
    struct Deferred {
        std::vector<Readback> readbacks;
        __strong id<MTLCommandBuffer> buffer = nil; // the committed buffer with the blits; nil: nothing to wait for
        std::function<void()> then;
        u32 read_addr = 0, read_size = 0;   // what `then` reads (an NV0039 source)
        u32 write_addr = 0, write_size = 0; // what `then` writes
    };
    std::deque<Deferred> deferred;
    bool readback_supported(const Surface& s);
    Readback encode_readback(Surface& s);
    void store_readback(const Readback& r);
    // The RSX reads [addr, addr + size) (or samples or renders to a surface there), or with `write` writes it: the
    // queued actions up to the last one that touches it run first, waiting for the GPU. A read conflicts with what
    // they write; a write also with what they read and with the surfaces they write back.
    usize deferred_conflict(u32 addr, u32 size, bool write) const; // how many queued actions must run first
    void before_access(u32 addr, u32 size, bool write);
    void run_deferred(bool wait, usize count = ~usize{0});
    void end_pass_over(u32 addr, u32 size); // a target there with only a clear so far: its pass ends (it is newer then)
    void check_cpu_writes(Surface& s);      // a watched copy destination the CPU wrote since: guest memory is newer
    // PSPOCKET_RSX_CHECK_SURFACES (a diagnostic): each surface keeps a hash of its guest memory from when it was made or
    // last written back; a texture sampled from a surface whose memory changed since (a CPU or SPU write the backend
    // was not told of) is logged.
    bool check_surfaces = false;
    void note_guest(Surface& s);
    void check_guest(const Surface& s, const TextureInfo& t, u32 unit);
    void upload_surface_from_guest(Surface& s);
    void grow_surface(Surface& outer, Surface& inner); // D-120
    void upload_surface_bytes(Surface& s); // the formats store_readback writes, but A8R8G8B8 (D-110)
    bool newer_in_guest(const PassTargets& t) const; // a target whose guest memory is newer: reloaded by open_pass
    void remove_surface(Surface* s);
    id<MTLTexture> feedback_copy(Surface& s); // D-148
    // A depth buffer as the texture of decode `mode` sees it (rsx_depth_to_texture), converted on the GPU when the
    // buffer was drawn to since; nil when it cannot be.
    Surface::Conversion* depth_as_texture(Surface& d, u32 mode);
    Surface* depth_sampled_directly(const TextureInfo& t);
    Surface::Conversion* color_as_texture(Surface& s, u32 mode);
    id<MTLRenderPipelineState> blit_pipeline_for(MTLPixelFormat format);
    id<MTLTexture> sample_view(id<MTLTexture> tex, std::unordered_map<u32, id<MTLTexture>>& views,
        const std::array<u8, 4>& src); // src: texture_channel_sources, adjusted for what the texture holds

    // ---- textures (texture_cache.mm) ----
    id<MTLTexture> bind_texture(const TextureInfo& t, u32 unit, bool& ok);
    // `overlay`: a GPU-newer target over the texture's memory whose pixels go into the decode on the GPU (D-137).
    CachedTexture* decode_texture(const TextureInfo& t, u32 ea, bool force_compute_dxt, std::string* error,
        Surface* overlay = nullptr);
    // D-137: whether a target's pixels can be laid over guest memory on the GPU (else it is written back), and the
    // encoding of that into `staging` (a copy of `size` bytes of guest memory from `ea`), on the render buffer.
    bool overlay_supported(const Surface& s);
    void encode_overlay(Surface& s, const TransientAllocator::Allocation& staging, u32 ea, u32 size);
    id<MTLSamplerState> sampler_for(const TextureInfo& t);
    id<MTLTexture> view_2d(id<MTLTexture> tex, u32 face, u32 level);
    bool read_texture(id<MTLTexture> tex, u32 face, u32 level, u32 w, u32 h, std::vector<std::array<float, 4>>& out);

    // ---- programs and pipelines (pipeline_cache.mm) ----
    const CompiledProgram* vertex_program_for(const RsxState& state, bool points);
    const CompiledProgram* fragment_program_for(const RsxState& state, const shader::FpOptions& options,
        const shader::FragmentProgram** program_out, u32& program_ea);
    std::optional<CompiledProgram> compile(const shader::TranslateResult& tr, const char* what);
    id<MTLRenderPipelineState> pipeline_for(const CompiledProgram& vp, const CompiledProgram& fp, const RsxState& state);
    id<MTLDepthStencilState> depth_state_for(const RsxState& state);
    id<MTLRenderPipelineState> clear_pipeline_for(u32 color_mask, bool depth, bool stencil);
};

// Format mapping helpers (surface_cache.mm)
MTLPixelFormat color_surface_format(u32 gcm_format);
MTLPixelFormat depth_surface_format(u32 gcm_format);
u32 surface_bytes_per_pixel(u32 gcm_format, bool depth);
// Whether two pitch-linear surfaces (address, pitch, bytes per row, rows) share pixels (surface_cache.mm).
bool surfaces_overlap(u32 a, u32 a_pitch, u32 a_row, u32 a_rows, u32 b, u32 b_pitch, u32 b_row, u32 b_rows);
// Which shader output channels land in the surface's channels (B8, G8B8 remaps).
std::array<u8, 4> surface_output_swizzle(u32 gcm_format);

} // namespace pspocket::rsx::metal
