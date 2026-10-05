#pragma once

// Internal Objective-C++ state of the Metal backend, shared by the .mm files in metal/src. Not a public header.

#if !__has_feature(objc_arc)
#error "the Metal backend is compiled with ARC (-fobjc-arc / CLANG_ENABLE_OBJC_ARC)"
#endif

#import <Metal/Metal.h>

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

private:
    static constexpr u32 kChunkSize = 4u << 20;
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
    std::unordered_map<u32, id<MTLTexture>> views; // swizzled views for sampling, by encoded channel sources
    u64 generation = 0;         // writes to it: colour, or depth for a depth buffer (D-105)
    u64 stencil_generation = 0; // writes to a depth buffer's stencil
    // A depth buffer (D-088) or colour target (D-098) read as a texture of another format: its conversion per decode
    // mode, the generation it was made from, and its swizzled views.
    struct Conversion {
        id<MTLTexture> texture = nil;
        u64 generation = 0;
        u64 stencil_generation = 0;
        std::unordered_map<u32, id<MTLTexture>> views;
    };
    std::unordered_map<u32, Conversion> conversions;
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
    u64 checked = ~0ull; // the check epoch (MetalBackend::Impl::check_epoch) it was last found clean in (D-104)
};

// ---- pipelines ----------------------------------------------------------------------------------------------------

struct CompiledProgram {
    id<MTLFunction> function = nil;
    shader::MslShader shader;
    std::vector<u32> ucode; // instruction words (FP: constants masked) for collision checks
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

struct CompiledProgram;

struct FragmentProgramEntry {
    u32 address = 0;
    shader::FragmentProgram program;
    WriteTracker::Watch watch{};
    u64 checked = ~0ull; // as CachedTexture::checked
    // What the program compiled to with these options, until it is decoded again (D-109): the lookup by hash and
    // ucode is not repeated every draw.
    std::vector<std::pair<shader::FpOptions, const CompiledProgram*>> compiled;
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
    const char* bind_source = ""; // how bind_texture served the last texture (for the trace)

    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLLibrary> kernels = nil;
    id<MTLComputePipelineState> decode_pipeline = nil;
    id<MTLComputePipelineState> read_pipeline = nil;
    id<MTLComputePipelineState> depth_pipeline = nil; // rsx_depth_to_texture
    id<MTLComputePipelineState> color_pipeline = nil; // rsx_color_to_texture
    id<MTLComputePipelineState> decode3d_pipeline = nil; // rsx_decode_texture3d
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
        bool operator==(const PassTargets& o) const
        {
            return color == o.color && color_count == o.color_count && depth == o.depth;
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
    // Why render passes ended (a started pass only), for the same report: the pass loads and stores its targets.
    enum PassEnd : u8 { kEndTargets, kEndTargetCopy, kEndDepthConversion, kEndColorConversion, kEndSubmit, kEndFlip,
                        kEndSurfaceGone, kEndFlush, kEndOther, kEndCount };
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
    __strong id<MTLBuffer> visibility = nil; // the render buffer's counters
    u32 visibility_next = 0;
    bool zpass_on = false;
    std::vector<VisibilitySlot> zpass_slots; // the counters of the query since the last clear
    s32 pass_zpass_slot = -1;                // this pass's counter for that query, -1 until a draw counts
    s32 encoder_zpass_slot = -1;             // the counter the encoder counts into, -1 when it does not count
    std::vector<PendingReport> pending_reports;
    PassTargets pass{};
    PassTargets draw_targets{}; // targets of the draw being prepared (render-target feedback detection)
    bool pass_open = false;    // targets chosen (descriptor built)
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
    std::unordered_map<u64, id<MTLDepthStencilState>> depth_states;
    std::unordered_map<u64, id<MTLSamplerState>> samplers;
    std::unordered_map<u64, id<MTLRenderPipelineState>> clear_pipelines;
    std::unordered_map<u32, id<MTLRenderPipelineState>> blit_pipelines; // by destination MTLPixelFormat
    std::unordered_map<u32, FragmentProgramEntry> fragment_programs; // by guest EA
    std::optional<shader::VertexProgram> vertex_program;             // decoded at the last dirty VP
    std::array<const CompiledProgram*, 2> vertex_compiled{};          // it compiled, without and with point size (D-109)
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
    void evict_overlapping(u32 address, u32 pitch, u32 row_bytes, u32 rows, const Surface* keep);
    bool read_back(Surface& s, std::vector<u8>& raw, bool stencil = false); // raw texture bytes, tightly packed
    bool write_back(Surface& s, const char* why);     // to guest memory, in the surface's format
    bool write_back_depth(Surface& s, const char* why);

    // Deferred readbacks (D-107): a surface's texels blitted into staging buffers in a render buffer committed at once,
    // written to guest memory once it has completed, then an action of the RSX's; later actions queue behind them.
    struct SurfaceImage { // what a write-back needs of a surface (the surface itself may be gone by then)
        u32 address = 0, pitch = 0, width = 0, height = 0, gcm_format = 0;
        bool depth = false;
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
    void upload_surface_from_guest(Surface& s);
    void upload_surface_bytes(Surface& s); // the formats store_readback writes, but A8R8G8B8 (D-110)
    bool newer_in_guest(const PassTargets& t) const; // a target whose guest memory is newer: reloaded by open_pass
    void remove_surface(Surface* s);
    id<MTLTexture> copy_texture(id<MTLTexture> src);
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
    CachedTexture* decode_texture(const TextureInfo& t, u32 ea, bool force_compute_dxt, std::string* error);
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
