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
#include "pspocket/rsx/shader/interp.hpp"
#include "pspocket/rsx/shader/ir.hpp"
#include "pspocket/rsx/shader/msl.hpp"
#include "pspocket/rsx/texture.hpp"
#include "pspocket/core/edge_job.h"
#include "pspocket/core/edge_registry.hpp"
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
    // The chunks waiting to be reused, and those of the buffer being recorded (PSPOCKET_RSX_MEMTRACE, D-1047).
    usize free_chunks() const
    {
        std::lock_guard lock(*free_mutex_);
        return free_->size();
    }
    usize in_use() const { return in_use_.size() + own_.size(); }

private:
    static constexpr u32 kChunkSize = 4u << 20;
    // Chunks kept for reuse at most (D-1047): a peak above it is given back to the system.
    static constexpr usize kMaxFreeChunks = 128;
    u64 generation_ = 0;
    u64 bytes_in_use_ = 0;
    id<MTLDevice> device_ = nil;
    id<MTLBuffer> current_ = nil;
    u32 offset_ = 0;
    std::vector<id<MTLBuffer>> in_use_; // 4 MB chunks: back to the free list when their command buffer completes
    std::vector<id<MTLBuffer>> own_;    // large uploads' own buffers: released then (D-1047)
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
    u64 mirror_generation = ~0ull; // its generations when last mirrored into guest memory (D-1030)
    // A depth buffer (D-088) or colour target (D-098) read as a texture of another format: its conversion per decode
    // mode, the generation it was made from, and its swizzled views.
    struct Conversion {
        id<MTLTexture> texture = nil;
        u64 generation = 0;
        u64 stencil_generation = 0;
        std::unordered_map<u32, id<MTLTexture>> views;
        u64 bind_epoch = 0, cache_epoch = 0; // a feedback copy's: the targets and texture cache it was made in (D-947)
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
    // The GPU-newer targets laid over guest memory when it was decoded, in the order laid (D-137; every one the
    // texture overlaps, D-214): the texture is out of date once one of them is drawn to again, or the set changes.
    struct Overlay {
        u64 serial = 0, generation = 0, stencil_generation = 0;
        bool operator==(const Overlay&) const = default;
    };
    std::vector<Overlay> overlays;
    // Every texel came from those targets (D-214): nothing was copied from guest memory and nothing there is watched;
    // the texture holds while the overlays do (a CPU write there makes a target older than memory, and then it is not
    // laid over and the texture is decoded from memory again).
    bool from_target = false;
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


// Samples per pixel across and down of a surface AA mode (NV4097_SET_SURFACE_FORMAT bits 12-15): 0 one sample, 3
// two across (diagonal centred), 4 and 5 two by two (square centred, square rotated); RPCS3 RSXThread.cpp aa_factor_u/v.
// Anything else is no RSX mode: one sample, `unknown` (D-250).
struct AaFactors {
    u32 x = 1, y = 1;
    bool unknown = false;
};
inline AaFactors aa_factors(u32 antialias)
{
    switch (antialias) {
    case 0: return {1, 1, false};
    case 3: return {2, 1, false};
    case 4:
    case 5: return {2, 2, false};
    default: return {1, 1, true};
    }
}

struct PipelineKey {
    u64 vp = 0, fp = 0;
    std::array<u32, 4> color_formats{};
    u32 depth_format = 0;
    u32 stencil_format = 0;
    std::array<u32, 4> write_masks{};
    std::array<u32, 4> blend{}; // enable | factors | ops packed per target (see pipeline_cache.mm)
    std::array<u32, 4> blend_ops{};
    bool operator<(const PipelineKey& o) const;
    bool operator==(const PipelineKey& o) const = default;
};
struct PipelineKeyHash { // D-953
    size_t operator()(const PipelineKey& k) const;
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

// A fragment program, by its content (D-240): decoded once, whatever address it is found at.
struct FragmentProgramEntry {
    u32 address = 0; // where it was first found
    shader::FragmentProgram program;
    // What the program compiled to with these options (D-109): the lookup by hash and ucode is not repeated every
    // draw.
    std::vector<std::pair<shader::FpOptions, const CompiledProgram*>> compiled;
    // The program's bytes as decoded, its embedded constants zeroed: what tells whether one found again is this one
    // (D-210, D-211).
    std::vector<u8> code;
};

// The program found last at a guest address (D-240): GTA V writes its programs into memory it reuses, so one address
// holds program after program; each found again, at any address, is the one decoded then.
struct FragmentProgramSlot {
    FragmentProgramEntry* entry = nullptr;
    u64 checked = ~0ull; // as CachedTexture::checked
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
    id<MTLComputePipelineState> memory_surface_pipeline = nil; // D-1025: guest bytes into a target (rsx_memory_to_surface)
    id<MTLComputePipelineState> thumbnail_pipeline = nil;      // D-1027: the presented image as 64x36 (rsx_thumbnail)
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
    u16 consts_lo = 0xffff, consts_hi = 0;           // the constants that upload holds (D-278)

    // EDGE geometry on the GPU (edge_geometry.mm, D-988): the draws' work items run by one compute dispatch per
    // transient buffer in a command buffer committed before the render buffer (submit).
    struct EdgeDraw {
        u32 shadow = 0, index_bytes = 0, blob = 0, scratch = 0, args = 0, indices = 0; // in the draw's allocation
    };
    struct EdgeJob {
        __strong id<MTLBuffer> buffer;
        u32 d[6]; // blob, shadow (in the buffer), index_bytes, scratch, args, indices (in the buffer)
    };
    id<MTLComputePipelineState> edge_pipeline = nil;
    u32 edge_threads = 128;
    std::vector<EdgeJob> edge_jobs;
    std::shared_ptr<std::atomic<u64>> edge_gpu_ns = std::make_shared<std::atomic<u64>>(0);
    struct EdgeStats {
        u64 draws = 0, jobs = 0, dispatches = 0, blob_bytes = 0, refused = 0;
    } stats_edge;
    EdgeStats report_edge{};
    u64 report_edge_gpu_ns = 0;
    edge::Stats report_edge_stats{};
    // PSPOCKET_EDGE_GPU=ab: EDGE on the GPU in every other PSPOCKET_EDGE_AB frames (default 30), each half's GPU time,
    // RSX thread CPU time and EDGE SPU job time summed apart and logged every 20 halves.
    u32 edge_ab_period = 0;
    usize edge_ab_half = 0; // the half of the frame being recorded (1: on the GPU)
    std::shared_ptr<std::array<std::atomic<u64>, 2>> edge_ab_gpu_ns = std::make_shared<std::array<std::atomic<u64>, 2>>();
    std::array<u64, 2> edge_ab_frames{}, edge_ab_cpu_ns{}, edge_ab_spu_ns{}, edge_ab_jobs{}, edge_ab_draws{};
    u64 edge_ab_cpu_last = 0, edge_ab_spu_last = 0, edge_ab_jobs_last = 0, edge_ab_draws_last = 0;
    void edge_ab_flip();
    // PSPOCKET_EDGE_GPU=verify: each draw's job ran on its SPU; its work item runs on the GPU beside it and the outputs
    // are compared once the GPU is done (the job's own output copied at the draw).
    struct EdgeCheck {
        __strong id<MTLBuffer> buffer;
        u32 base = 0; // the job's region in it
        EdgeDraw e;
        u32 drawn = 0; // the indices the job's hole draws (its kept ones)
        u32 guest_index_bytes = 0;
        std::vector<u8> guest; // its allocation as the job put it
        ps_edge_job job{};
        u64 kinds = 0;
    };
    std::vector<EdgeCheck> edge_checks;
    struct EdgeVerify {
        std::atomic<u64> jobs{0}, counts_same{0}, lists_same{0}, vertices{0}, vertices_same{0}, int_diff{0};
        std::atomic<u64> f32_ulps{0};          // the largest F32 difference in ulps
        std::atomic<u64> f32_rel_e9{0};        // ... against the value's magnitude, times 1e9
        std::atomic<u64> kept_gpu{0}, kept_spu{0};
        // by kind (WorkItem::kinds' bits: the reasons, and edge::kind_route of the routes)
        std::array<std::atomic<u64>, 64> kind_jobs{}, kind_lists_same{}, kind_vertices{}, kind_vertices_same{};
    };
    std::shared_ptr<EdgeVerify> edge_verify_stats = std::make_shared<EdgeVerify>();
    void edge_verify(std::unique_ptr<edge::WorkItem> item, u32 drawn);
    static void edge_check(const EdgeCheck& c, EdgeVerify& v); // one job's GPU output against what its SPU put
    bool edge_init();
    std::unique_ptr<edge::WorkItem> edge_take(u32 index_ea);
    u32 edge_layout(const edge::WorkItem& item, u32 at, EdgeDraw& out);
    void edge_queue(const edge::WorkItem& item, const TransientAllocator::Allocation& a, const EdgeDraw& e);
    void edge_flush();

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
        bool whole = false; // the timestamp and the ready word too, the ready word last (D-1021)
    };
    static constexpr u32 kVisibilitySlots = 4096;
    u32 visibility_slots = kVisibilitySlots; // a render buffer's counters: doubled when a buffer runs out (D-1024)

    // GPU time of the render buffers (completion handlers add GPUEndTime - GPUStartTime), logged with the frame's draws
    // and passes every kGpuReportFlips flips: what a slower GPU (the iPhone's) has to fit in a frame.
    static constexpr u64 kGpuReportFlips = 300;
    std::shared_ptr<std::atomic<u64>> gpu_ns = std::make_shared<std::atomic<u64>>(0);
    // PSPOCKET_GPU_AB=N (a measuring aid, D-272): a change under test, written to apply only where ab_on(), is on in
    // every other run of N frames, and the GPU time of each half is summed apart, so both see the same scenes (each
    // command buffer's span, which overlap: compare the halves, not with other runs). ab_frames counts each half's
    // frames; ab_cpu_ns sums the RSX thread's CPU time of each half's frames (D-278). Nothing calls ab_on() between
    // experiments.
    u32 ab_period = 0;
    std::shared_ptr<std::array<std::atomic<u64>, 2>> ab_ns = std::make_shared<std::array<std::atomic<u64>, 2>>();
    std::array<u64, 2> ab_frames{};
    std::array<u64, 2> ab_cpu_ns{}; // the RSX thread's CPU time of each half's frames (D-278)
    u64 ab_cpu_last = 0;
    // Each half's render passes and attachment traffic (D-945), and the counters at the last flip.
    std::array<u64, 2> ab_passes{}, ab_load_bytes{}, ab_store_bytes{};
    std::array<u64, 3> ab_last{};
    s32 submit_half = -1; // the flip's submit: the half of the frame its buffer holds (ab_on() has moved on by then)
    // PSPOCKET_RSX_CPU_AB=N (D-953): the same halves, but what they switch is the RSX thread's per-draw fast paths
    // (fast()), the GPU-side experiments staying on in both; each half's draws are counted for its CPU time a draw.
    bool ab_cpu = false;
    std::array<u64, 2> ab_draws{};
    u64 ab_last_draws = 0;
    // Each half's program lookups (D-953): fragment programs found in their own list or not; vertex programs looked
    // up, decoded, and compared with one decoded before.
    std::array<u64, 2> fp_variant_hits{}, fp_variant_misses{}, vp_lookups{}, vp_decodes{}, vp_compares{};
    usize ab_half() const { return ab_period && (stats.flips / ab_period) % 2 == 1 ? 1 : 0; }
    bool ab_on() const { return !ab_period || ab_cpu || (stats.flips / ab_period) % 2 == 1; } // the change applies
    // The fast paths by item (PSPOCKET_RSX_CPU_AB=N:MASK switches only those in MASK; all by default).
    enum FastPath : u32 {
        kFastLookups = 1,   // surfaces by address, pipelines by hash, one take a draw, the fragment program's list (D-953)
        kFastSetters = 4,   // encoder state set only when it changes (D-954)
        kFastScratch = 8,   // the draw's lists kept from draw to draw (D-956)
        kFastIndexScan = 16, // the index scan in NEON (D-957)
        kFastSurfacePool = 32 // a removed surface's texture for the next of its format and size (D-984)
    };
    u32 ab_items = ~0u;
    bool fast(u32 item = ~0u) const { return !ab_cpu || !(ab_items & item) || ab_half() == 1; }
    // Why render passes ended (a started pass only), for the same report: the pass loads and stores its targets. The
    // copies between targets are told apart (D-148): an NV0039 copy on the GPU, a target sampled while drawn to, a
    // corner of a larger target, pixels between a target and its parent, a target grown over a smaller one.
    enum PassEnd : u8 { kEndTargets, kEndGpuCopy, kEndFeedbackCopy, kEndCornerCopy, kEndRegionCopy, kEndGrow,
                        kEndDepthConversion, kEndColorConversion, kEndTargetOverlay, kEndSubmit, kEndFlip, kEndSurfaceGone,
                        kEndFlush, kEndClear, kEndOther, kEndCount };
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
    u64 report_decoded = 0, report_decoded_overlay = 0, report_decoded_written = 0;
    u64 report_faults = 0; // the tracker's write faults at the last report (D-134)
    u64 report_overlays = 0;
    u64 report_programs = 0, report_pipelines = 0, report_compile_ns = 0, report_draws_compiling = 0;
    u64 report_draws_interpreted = 0;
    u64 report_feedback = 0, report_corners = 0, report_reloads = 0, report_gpu_wait_ns = 0;
    u64 report_pooled = 0; // D-956
    AlarmCounts report_alarms{}; // D-1022
    u64 report_carried = 0;      // D-1025
    u64 report_depth_bounds = 0; // D-261
    u64 report_feedback_reused = 0, report_feedback_first = 0, report_feedback_invalidated = 0;
    u64 report_feedback_rebound = 0, report_feedback_strict = 0, report_cache_epoch = 0; // D-947
    u64 report_load_bytes = 0, report_store_bytes = 0, report_clear_only = 0, report_clear_only_bytes = 0; // D-273
    u64 report_store_skipped = 0, report_clears_folded = 0, report_clears_waited = 0, report_clear_passes = 0; // D-945
    // At most config.frames_in_flight frames queued for the GPU (D-148): a flip waits for the oldest to complete. Each
    // frame queued holds what it allocated until then, and nothing else held the RSX thread back.
    dispatch_semaphore_t frames_in_flight = nil;
    u32 frames_in_flight_limit = 0;
    std::atomic<bool> trim_requested{false}; // MetalBackend::trim_memory
    std::atomic<bool> trace_requested{false}; // MetalBackend::trace_next_frame
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
    // The RSX's texture cache over a target a draw samples while drawing to it (D-947): the copy is the cache, kept
    // while the draws go on to the same targets and the game does not invalidate the texture cache. The targets the
    // draws went to (bind_epoch counts their changes) and the state's texture_cache_epoch at this draw.
    PassTargets bind_targets{};
    u64 bind_epoch = 0;
    u64 cache_epoch = 0;
    bool feedback_cached = true; // PSPOCKET_RSX_STRICT_FEEDBACK: off, a copy after every write (D-148)
    u8 shader_blend = 0; // the draw's colour attachments its fragment shader blends (D-948): no blending in the pipeline
    // For the report: the fragment program and unit of the texture being bound, and the copies by where they came from.
    u32 feedback_fp = 0, feedback_unit = 0;
    struct FeedbackSource {
        u64 made = 0, reused = 0;
    };
    // By the draw's fragment program and unit and the target's address, size and format; named only when reported
    // (D-956: a name formatted for every copy and every draw that read a kept one).
    using FeedbackKey = std::array<u32, 6>;
    struct FeedbackKeyHash {
        size_t operator()(const FeedbackKey& k) const
        {
            u64 h = 0;
            for (u32 v : k) h = (h ^ v) * 0x100000001b3ull;
            return static_cast<size_t>(h);
        }
    };
    std::unordered_map<FeedbackKey, FeedbackSource, FeedbackKeyHash> feedback_sources; // this report window's
    bool pass_open = false;    // targets chosen (descriptor built)
    bool pass_partial = false; // the pass draws in part of an attachment: its load-action clear would clear the rest
    bool pass_started = false; // encoder created
    u32 pass_width = 0, pass_height = 0;

    // ---- fewer render passes (D-945) ----
    // A clear that waits: one of targets the open pass does not draw to, made while it draws (the pass goes on); one
    // of whole targets the open pass does not have (the next pass may have them); or the load-action clears of a pass
    // that never drew, when the targets change. It runs as the load action of the next pass that draws all of its
    // surfaces (a clear of whole surfaces), else in a render pass of its own right before the next pass starts or any
    // other GPU work is recorded, and before anything reads its surfaces (bind_texture, depth_sampled_directly) or
    // writes them (a reload from guest memory). What it changes in its surfaces' state (initialized, newer on the GPU,
    // generations, D-122's parent and children) is set when it is made, as if it had run: a write-back, conversion or
    // copy of one of them is GPU work, so the clear runs before it.
    struct ClearOp {
        PassTargets targets;                   // its attachments: colour ones in order, then depth
        u32 width = 0, height = 0;             // the area a pass over them draws in
        u32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;    // the region it clears there
        std::array<MTLClearColor, 4> colors{}; // per colour attachment, as the surface stores it
        u32 channels = 0xf;                    // colour channels cleared, R G B A as bits 0 to 3
        double depth = 1.0;
        u32 stencil = 0;
        bool clear_depth = false, clear_stencil = false;
        bool whole = false; // every attachment cleared whole, all its channels: a load action does it
        // A clear of part of them: the attachments that held something load it, the others start cleared.
        std::array<bool, 4> color_loaded{};
        bool depth_loaded = false;
    };
    std::vector<ClearOp> pending_clears;
    // The pass the targets changed away from, its encoder not ended yet (D-945): its store actions were left unknown and
    // are set when the next encoder is made. When that is the next pass, an attachment it or a waiting clear before it
    // clears whole is not stored: nothing can have read it in between, since any other GPU work ends this encoder
    // first and stores everything (render_buffer()).
    struct Closing {
        id<MTLRenderCommandEncoder> encoder = nil;
        PassTargets targets;
        u32 width = 0, height = 0;
    };
    Closing closing;
    bool pass_store_unknown = false; // the open pass's store actions are set when it ends (D-945)
    // Clears of other targets made while the open pass drew: counted (BackendStats::clears_waited) when the pass then
    // draws on, so the pass ended no sooner for them; forgotten when it ends.
    u32 clears_waiting_on_pass = 0;
    // On unless configured off, and in the "on" half of PSPOCKET_GPU_AB's frames; never with PSPOCKET_GPU_PASS_TIMES,
    // which times each pass alone.
    bool merging() const { return config.merge_passes && ab_on() && !pass_times; }
    static void target_area(const PassTargets& t, u32& width, u32& height, bool& partial); // as open_pass makes it
    void wait_pass_clears();     // end_pass: a pass that only cleared, its targets changing: its clears wait
    void fold_pending_clears();  // open_pass: the waiting clears the new pass does as its load actions
    void flush_pending_clears(); // each waiting clear runs in a pass of its own, in the order they were made
    void settle_before_pass();   // start_encoder: the pass before learns what it need not store; the clears run
    // Ends the closing pass's encoder. `next_pass`: the open pass is about to start, and the waiting clears to run
    // before it: what they clear whole is not stored; else everything is.
    void finish_closing(bool next_pass);
    // Ends a render encoder; with its store actions left unknown, an attachment in `dont_store` is not stored (bits 0
    // to 3 the colour attachments, 4 the depth, 5 the stencil), the rest is.
    void end_encoder(id<MTLRenderCommandEncoder> enc, const PassTargets& t, u32 width, u32 height, bool unknown,
        u32 dont_store);
    bool clear_waits_on(const Surface* s) const;
    bool clear_waits_over(u32 addr, u32 size) const;
    void run_clear_op(const ClearOp& op);
    // A clear's quad through the scissor: the colour attachments of `t` in the `cleared` bit mask take `channels`, the
    // others nothing (a clear of some of the targets of a pass with more, D-945).
    void encode_clear_quad(id<MTLRenderCommandEncoder> enc, const PassTargets& t, u32 cleared, u32 width, u32 height,
        u32 x0, u32 y0, u32 x1, u32 y1, MTLClearColor color, u32 channels, double depth, u32 stencil, bool clear_depth,
        bool clear_stencil);
    // Waiting clears of part of the open pass's attachments it does as its first draws, once its encoder starts
    // (fold_pending_clears; in order, after its load actions).
    std::vector<ClearOp> pass_quads;
    // A clear waits: one of whole surfaces makes the earlier waiting clears of them moot (nothing read them since).
    void push_pending_clear(ClearOp op);
    static void drop_target(ClearOp& op, const Surface* s); // a clear that waits no longer clears `s`

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
    u64 surfaces_version = 1; // bumped as surfaces are made and removed (their ranges never change)
    // D-1056: the surfaces' byte ranges by start, each with the highest end up to it, for the reads below.
    struct SurfaceSpan {
        u64 lo = 0, hi = 0, reach = 0;
        Surface* s = nullptr;
    };
    std::vector<SurfaceSpan> surface_spans;
    u64 spans_version = 0;
    std::vector<Surface*> stale_scratch;
    bool stale_sync = true; // PSPOCKET_RSX_NO_STALE_SYNC=1: the reads below only count the alarm (tests, A/B)
    // Textures of surfaces removed, for the next surface of the same format and size (D-984): GTA V's 640x360
    // target and the 320x180 F16 one over the same memory replace each other every frame, a new texture each time.
    // At most kSurfacePoolMax, oldest dropped first; emptied by trim().
    static constexpr usize kSurfacePoolMax = 24;
    std::deque<id<MTLTexture>> surface_texture_pool;
    u64 report_surfaces_made = 0, report_surfaces_reused = 0; // at the last report
    // The surfaces at each guest address, in the order they were made (D-953): a surface's address is set when it is
    // made (make_surface) and it leaves only through remove_surface. The lookups by address read these instead of
    // scanning every surface (some 150 in GTA V, up to four scans for each texture a draw binds); they meet them in
    // the order the scans did.
    std::unordered_map<u32, std::vector<Surface*>> surfaces_at;
    const std::vector<Surface*>& surfaces_at_address(u32 address) const
    {
        static const std::vector<Surface*> kNone;
        const auto it = surfaces_at.find(address);
        return it == surfaces_at.end() ? kNone : it->second;
    }
    // Calls f on each surface at `address` in that order until it returns true; the surface it stopped at, or null.
    template <class F>
    Surface* visit_surfaces_at(u32 address, F&& f) const
    {
        if (!fast(kFastLookups)) { // the scan, for PSPOCKET_RSX_CPU_AB's other half
            for (const auto& s : surfaces)
                if (s->address == address && f(s.get())) return s.get();
            return nullptr;
        }
        for (Surface* s : surfaces_at_address(address))
            if (f(s)) return s;
        return nullptr;
    }
    std::unordered_map<u64, std::unique_ptr<CachedTexture>> textures; // key: hash of decode inputs
    std::unordered_set<u64> reinterpretations; // (target format, texture format, width) pairs logged once
    // By hash, and the last one found again first (D-953): an ordered map's compares of the 72-byte key were most of
    // each draw's lookup.
    std::unordered_map<PipelineKey, id<MTLRenderPipelineState>, PipelineKeyHash> pipelines;
    std::map<PipelineKey, id<MTLRenderPipelineState>> pipelines_ordered; // PSPOCKET_RSX_CPU_AB's other half
    PipelineKey last_pipeline_key{};
    id<MTLRenderPipelineState> last_pipeline = nil; // nil: none; forgotten whenever `pipelines` changes
    void add_pipeline(const PipelineKey& key, id<MTLRenderPipelineState> pso)
    {
        pipelines[key] = pso;
        pipelines_ordered[key] = pso;
        last_pipeline = nil;
    }
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
    std::unordered_map<u32, FragmentProgramSlot> fragment_programs; // by guest EA
    // Texture layouts by shape (layout_for): format, width, height, depth, mipmaps, dimension, cubemap; pitch, border.
    struct TextureLayoutKey {
        u64 shape = 0;
        u64 pitch = 0; // and the border flag above it
        bool operator==(const TextureLayoutKey&) const = default;
    };
    struct TextureLayoutKeyHash {
        size_t operator()(const TextureLayoutKey& k) const { return std::hash<u64>()(k.shape * 0x9E3779B97F4A7C15ull ^ k.pitch); }
    };
    std::unordered_map<TextureLayoutKey, std::optional<TextureLayout>, TextureLayoutKeyHash> texture_layouts;
    std::optional<TextureLayout> scratch_layout; // layout_for's answer for a shape the key cannot hold
    // Every fragment program decoded, by hash (more than one only when two programs' hashes are equal); kept while
    // the slots point into them, and all forgotten past kMaxFragmentPrograms (decoded again as they come).
    std::unordered_map<u64, std::vector<std::unique_ptr<FragmentProgramEntry>>> fragment_program_bodies;
    size_t fragment_program_count = 0;
    static constexpr size_t kMaxFragmentPrograms = 8192;
    shader::FpScan fp_scan; // reused (the RSX thread's)
    // Vertex programs decoded lately, the current one first (D-210). A game that uploads its programs again before
    // nearly every draw (Assassin's Creed's crowds: 2,150 draws a frame) finds the one it uploads here by its words
    // instead of decoding it again: decoding was 7% of the RSX thread.
    struct DecodedVertexProgram {
        shader::VertexProgram program;
        std::vector<u32> words;                         // its instructions' words, from its start to its end
        std::vector<std::pair<u64, const CompiledProgram*>> compiled; // its variants compiled, by what tells them apart (D-109, D-269)
        u8 texture_units = 0; // the vertex texture units its TXL instructions sample (D-274)
        // The transform constants it reads, c[const_lo] to c[const_hi] (all of them when it indexes them by the
        // address register); none when const_lo > const_hi (D-278).
        u16 const_lo = 0xffff, const_hi = 0;
    };
    static constexpr usize kDecodedVertexPrograms = 16;
    std::vector<DecodedVertexProgram> vertex_programs;
    bool vertex_program_ok = false; // vertex_programs.front() is the program the state holds
    void forget_program_lookups();                                   // a program key now names another program
    // The shader interpreter (D-940). vertex_program_for leaves the program it looked up and its options here, for a
    // draw the interpreter draws; the programs it runs are encoded once each (by hash), in buffers of their own.
    const shader::VertexProgram* interp_vp = nullptr;
    shader::VpOptions interp_vp_options;
    u8 interp_vp_textures = 0;
    std::unordered_map<u64, id<MTLBuffer>> interp_code;
    std::unordered_set<u64> interp_refused; // programs the interpreter refuses (as the translation does)
    bool force_interpreter = false; // config.force_interpreter or PSPOCKET_RSX_INTERPRETER
    bool interpreter_on() const { return async != nullptr && config.shader_interpreter; }
    // The interpreter's vertex function (vertex) or fragment variant for `mrt` colour outputs and depth export: made
    // in the background (as any program; the shader cache keeps them), nullptr until then. Synchronous without async.
    const CompiledProgram* interpreter_program(bool vertex, u32 mrt, bool depth);
    id<MTLBuffer> interpreter_code(u64 key, const std::vector<u32>& words);
    std::vector<id<MTLTexture>> outputs;
    u32 next_output = 0;

    // A draw's working lists, kept from draw to draw (D-956): made anew, they were a dozen heap allocations a draw.
    struct DrawScratch {
        struct Range {
            u32 first, count;
        };
        struct DrawnIndices {
            bool ok;    // Metal can run the range over its indices as they are (D-271)
            u32 offset; // of its first index in index_data's buffer
            u32 size;   // 2 or 4
        };
        struct Copy {
            u32 attr, ea, size;
        };
        struct Span {
            u32 ea, end, offset;
        };
        std::vector<Range> ranges;
        std::vector<u32> index_offsets;
        std::vector<u32> restarts;      // the indexed ranges' restart positions, range after range (D-249)
        std::vector<u32> restart_begin; // range r's are restarts[restart_begin[r] .. restart_begin[r + 1])
        std::vector<DrawnIndices> drawn_indices;
        std::vector<Copy> copies;
        std::vector<Span> spans;
        std::vector<std::array<float, 4>> fconst;
        std::vector<u32> list; // a range's index list (restarts, converted primitives)
        void clear()
        {
            ranges.clear();
            index_offsets.clear();
            restarts.clear();
            restart_begin.clear();
            drawn_indices.clear();
            copies.clear();
            spans.clear();
            fconst.clear();
            list.clear();
        }
    };
    DrawScratch draw_scratch;

    // The textures copies of targets are made into (D-956): a feedback copy (D-148, D-947) and a corner (D-120) go into
    // a new texture at each change, so the GPU never waits for the draws that read the old one. The old one waits here
    // until the frame that last used it has run, then serves the next copy of its format and size: making them was
    // some 3% of the RSX thread in GTA V (six of each a frame). One not taken within kPoolFrames frames goes.
    struct PooledTexture {
        __strong id<MTLTexture> texture;
        u64 frame; // the frame being recorded when it was put here: free once the frame event has reached it
    };
    static constexpr u64 kPoolFrames = 8;
    static constexpr usize kPoolTextures = 64;
    std::vector<PooledTexture> texture_pool;
    u64 frames_done = 0; // the frame event's value as last read
    id<MTLTexture> copy_texture(MTLPixelFormat format, u32 width, u32 height);
    void pool_texture(id<MTLTexture> texture);
    void trim_texture_pool(bool all);

    // The draw's texture units decoded once (D-954): the fragment units for its program options, then again for the
    // units it samples and their samplers; the vertex units its program samples when first wanted.
    std::array<TextureInfo, kFragmentTextureCount> frag_tex{};
    std::array<u32, kFragmentTextureCount> frag_tex_control{}; // texture_control() of the enabled ones
    std::array<TextureInfo, kVertexTextureCount> vert_tex{};
    u32 vert_tex_decoded = 0; // this draw's
    void decode_textures(const RsxState& state);
    const TextureInfo& vertex_texture(const RsxState& state, u32 t)
    {
        if (!(vert_tex_decoded & (1u << t))) {
            vert_tex[t] = state.vertex_texture(t);
            vert_tex_decoded |= 1u << t;
        }
        return vert_tex[t];
    }
    SurfaceInfo draw_surface{}; // the surface registers select_targets decoded for the draw

    // The draw encoder's state as the draws set it (D-954): a setter whose value the encoder already holds is left
    // out (each is a message send and the driver's own bookkeeping, nothing changed or not). Forgotten at each new
    // encoder and whenever something else sets state on it (encode_clear_quad, the shader interpreter's bindings).
    struct EncoderState {
        __strong id<MTLRenderCommandEncoder> encoder = nil; // held: no other encoder can take its address meanwhile
        __strong id<MTLRenderPipelineState> pso = nil;
        __strong id<MTLDepthStencilState> depth = nil;
        u32 stencil_front = ~0u, stencil_back = ~0u;
        bool viewport_set = false;
        MTLViewport viewport{};
        s32 clip_mode = -1, cull = -1, winding = -1, fill = -1;
        bool scissor_set = false;
        MTLScissorRect scissor{};
        bool bias_set = false;
        float bias = 0, slope = 0;
        bool blend_color_set = false;
        u32 blend_color = 0;
        std::array<__strong id<MTLBuffer>, 4> vbuf{}; // slot::kVsVertexData .. kVsIndexData
        std::array<NSUInteger, 4> voff{};
        std::array<__strong id<MTLTexture>, kFragmentTextureCount> ftex{};
        std::array<__strong id<MTLSamplerState>, kFragmentTextureCount> fsamp{};
        __strong id<MTLTexture> bounds_tex = nil;
        std::array<__strong id<MTLTexture>, kVertexTextureCount> vtex{};
        std::array<__strong id<MTLSamplerState>, kVertexTextureCount> vsamp{};
        std::vector<u8> fconst;        // the fragment constants' bytes as last set
        std::array<u8, sizeof(shader::MslFsParams)> fs{};
        bool fs_set = false;
    };
    EncoderState enc_state;
    void forget_encoder_state()
    {
        std::vector<u8> keep = std::move(enc_state.fconst); // its room, for the next
        enc_state = {};
        enc_state.fconst = std::move(keep);
        enc_state.fconst.clear();
    }

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
    void trace_probe(); // PSPOCKET_RSX_TRACE_PROBE (D-1028)

    // ---- surfaces (surface_cache.mm) ----
    Surface* get_color_surface(u32 address, u32 pitch, u32 width, u32 height, u32 format, bool swizzled);
    Surface* get_depth_surface(u32 address, u32 pitch, u32 width, u32 height, u32 format, bool depth_float);
    Surface* find_surface(u32 address, bool depth) const;
    Surface* make_surface(u32 address, u32 pitch, u32 width, u32 height, u32 format, bool depth, bool swizzled,
        MTLPixelFormat pf);
    // `made`: the new target (kept); `carried`: the victims whose bytes it took (D-1025): only what lies outside it is
    // lost, which the alarm counts (D-1022).
    void evict_overlapping(u32 address, u32 pitch, u32 row_bytes, u32 rows, const Surface* keep,
        const Surface* keep_too = nullptr, const Surface* made = nullptr, const std::vector<Surface*>& carried = {});
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
    // What the queued actions touch, together (D-953): an access outside it conflicts with none of them, and is not
    // compared with each (GTA V keeps some 250 queued, its labels behind readbacks, and a draw makes ten accesses).
    // Widened as actions queue, made again as they leave.
    u64 deferred_lo = ~0ull, deferred_hi = 0;
    void note_deferred(const Deferred& d)
    {
        const auto add = [&](u64 a, u64 n) {
            if (!n) return;
            deferred_lo = std::min(deferred_lo, a);
            deferred_hi = std::max(deferred_hi, a + n);
        };
        add(d.read_addr, d.read_size);
        add(d.write_addr, d.write_size);
        for (const Readback& r : d.readbacks) add(r.image.address, u64{r.image.pitch} * r.image.height);
    }
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
    // A read of guest memory [addr, addr + size) that does not take the GPU's copy of a target (D-1056): every
    // GPU-newer target with pixel bytes there, drawn or sampled in the last two flips and not in `handled`, is written
    // back first; one that cannot be counts the stale_gpu_reads alarm. Older ones are left (their memory is taken as
    // reused). True when any was found.
    bool sync_gpu_held(u32 addr, u32 size, const char* why, const std::vector<Surface*>& handled = {});
    // PSPOCKET_RSX_CHECK_SURFACES (a diagnostic): each surface keeps a hash of its guest memory from when it was made or
    // last written back; a texture sampled from a surface whose memory changed since (a CPU or SPU write the backend
    // was not told of) is logged.
    bool check_surfaces = false;
    // PSPOCKET_GPU_PASS_TIMES (a measuring aid): each render pass goes to the GPU in a command buffer of its own, which
    // the RSX thread waits for (the GPU overlaps command buffers otherwise), and its GPU time is summed by its place in
    // the frame; the passes that took most are logged every 300 frames. Much slower than usual: for finding where GPU
    // time goes, not for frame rates.
    struct PassTimes {
        struct Entry {
            u64 ns = 0, frames = 0, draws = 0;
            u64 vertex_ns = 0, fragment_ns = 0; // the pass's stages, from stage boundary timestamps when the GPU has them
            u64 before_ns = 0; // the command buffer of what was recorded between the pass before and this one
            std::string targets;
        };
        std::mutex mutex;
        std::map<u32, Entry> by_place;
        id<MTLCounterSet> timestamps = nil; // nil: the GPU has no stage boundary timestamps
        double gpu_tick_ns = 1.0;
    };
    std::shared_ptr<PassTimes> pass_times;
    u32 pass_place = 0;       // the pass's place in this frame
    id<MTLCounterSampleBuffer> pass_samples = nil; // the timed pass's four stage boundary timestamps
    u64 pass_draws_start = 0; // stats.draws when the pass started
    u64 pass_last_fp = 0;     // the hash of the last fragment program the pass drew with
    u64 skip_fp = 0;          // PSPOCKET_SKIP_FP: draws with this fragment program are skipped (measuring what one costs)
    bool pass_times_submitting = false;
    void note_guest(Surface& s);
    void check_guest(const Surface& s, const TextureInfo& t, u32 unit);
    void upload_surface_from_guest(Surface& s);
    void grow_surface(Surface& outer, Surface& inner); // D-120
    void upload_surface_bytes(Surface& s); // the formats store_readback writes, but A8R8G8B8 (D-110)
    bool newer_in_guest(const PassTargets& t) const; // a target whose guest memory is newer: reloaded by open_pass
    void remove_surface(Surface* s);
    // D-148; `depth_only`: a stencil written since does not matter (the depth bounds test reads the depth, D-261).
    id<MTLTexture> feedback_copy(Surface& s, bool depth_only = false);
    FeedbackKey feedback_source(const Surface& s) const; // a feedback copy's draw, for the report (D-947)
    static std::string feedback_source_name(const FeedbackKey& k);
    void log_pass_times();
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
    // compute_texture_layout's answer for a texture of this shape, kept (D-242): every bind asked twice, each time
    // building its levels' vector (run 221). Null when the layout is refused.
    const TextureLayout* layout_for(const TextureInfo& t);
    // `overlay`: a GPU-newer target over the texture's memory whose pixels go into the decode on the GPU (D-137).
    CachedTexture* decode_texture(const TextureInfo& t, u32 ea, bool force_compute_dxt, std::string* error,
        const std::vector<Surface*>& overlays = {});
    // The GPU-newer targets a linear texture decoded from memory at [ea, ea + size) with row pitch `pitch` overlaps
    // with the same pitch (side by side in its rows), besides `first` (the one at its address, laid over first): laid
    // over on the GPU when they can be, else written back (D-214). Oldest first.
    std::vector<Surface*> overlays_for(u32 ea, u32 size, u32 pitch, Surface* first);
    // D-137: whether a target's pixels can be laid over guest memory on the GPU (else it is written back), and the
    // encoding of that into `staging` (a copy of `size` bytes of guest memory from `ea`), on the render buffer.
    bool overlay_supported(const Surface& s);
    void encode_overlay(Surface& s, const TransientAllocator::Allocation& staging, u32 ea, u32 size);
    // Render to vertex buffer (D-1034): targets the GPU drew over a draw's vertex arrays, laid over their copies.
    std::vector<Surface*> vertex_overlay_scratch;
    void vertex_targets(const std::vector<DrawScratch::Span>& spans);
    void overlay_vertex_targets(const std::vector<DrawScratch::Span>& spans, const TransientAllocator::Allocation& data);
    // D-1025: a new target `s` over the memory of GPU-newer targets of other formats or sizes (`sources`, oldest
    // first) starts with what that memory would hold on the RSX: guest memory, the sources laid over it (as a texture
    // over them is decoded), into its texels, all on the GPU. False: a format this cannot do (nothing recorded).
    bool carry_into(Surface& s, const std::vector<Surface*>& sources);
    // The GPU-newer targets a new target at [address, rows of row_bytes `pitch` apart) lays over (D-1025): those
    // evict_overlapping removes, and `also` (a smaller one at its start it grows over, D-120), oldest first.
    std::vector<Surface*> carry_sources(u32 address, u32 pitch, u32 row_bytes, u32 rows, const Surface* keep,
        const Surface* keep_too, Surface* also);
    bool carry_on = true; // PSPOCKET_RSX_NO_CARRY=1: off, the targets' bytes are lost as before (D-1025)

    // ---- the exact mode (D-1029) ----
    bool exact = false;
    // No-copy buffers over RSX local memory (16 MB windows, or a target's own pages), which the GPU writes targets back
    // into with the overlay kernels (D-137).
    struct GuestWindow {
        __strong id<MTLBuffer> buffer = nil;
        u32 base = 0, size = 0;
    };
    std::vector<GuestWindow> guest_windows;
    id<MTLBuffer> guest_window(u32 addr, u32 size, u32& base);
    // Every GPU-newer target into guest memory (on the GPU where a window covers it, else a readback), then waits;
    // they are no longer newer on the GPU and their memory is watched for the CPU's writes.
    void exact_write_back();
    void mirror_targets(); // PSPOCKET_RSX_MIRROR (D-1030)
    void exact_defer(u32 addr, u32 size, std::function<void()> action); // D-1029
    u64 exact_write_backs = 0, exact_bytes = 0;
    // The draw signature (D-1029): draws drawn by fragment program hash, and a copy at each report for other threads.
    std::unordered_map<u64, u64> fp_draws;
    mutable std::mutex fp_draws_mutex;
    std::unordered_map<u64, u64> fp_draws_snapshot;
    u64 fp_draws_flips = 0;
    id<MTLSamplerState> sampler_for(const TextureInfo& t);
    id<MTLTexture> view_2d(id<MTLTexture> tex, u32 face, u32 level);
    bool read_texture(id<MTLTexture> tex, u32 face, u32 level, u32 w, u32 h, std::vector<std::array<float, 4>>& out);

    // ---- programs and pipelines (pipeline_cache.mm) ----
    // `fp_inputs`: the inputs the draw's fragment program reads (TranslatedShader::input_mask), `two_sided`: whether
    // it reads the back colours too (D-267).
    const CompiledProgram* vertex_program_for(const RsxState& state, bool points, u32 fp_inputs, bool two_sided);
    const CompiledProgram* fragment_program_for(const RsxState& state, const shader::FpOptions& options,
        const shader::FragmentProgram** program_out, u32& program_ea);
    std::optional<CompiledProgram> compile(const shader::TranslateResult& tr, const char* what);
    id<MTLRenderPipelineState> pipeline_for(const CompiledProgram& vp, const CompiledProgram& fp, const BlendInfo& blend,
        const ColorMaskInfo& masks);
    id<MTLDepthStencilState> depth_state_for(const DepthStencilInfo& ds);
    // The clear quad's pipeline for `t`'s attachments: the colour ones in the `cleared` bit mask written with
    // `color_mask`, the others not.
    id<MTLRenderPipelineState> clear_pipeline_for(const PassTargets& t, u32 cleared, u32 color_mask, bool depth,
        bool stencil);
    std::array<id<MTLDepthStencilState>, 4> clear_depth_states{}; // by clear_depth | clear_stencil << 1
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
