#pragma once

// Internal Objective-C++ state of the Metal backend, shared by the .mm files in metal/src. Not a public header.

#if !__has_feature(objc_arc)
#error "the Metal backend is compiled with ARC (-fobjc-arc / CLANG_ENABLE_OBJC_ARC)"
#endif

#import <Metal/Metal.h>

#include <array>
#include <deque>
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
    u64 generation = 0; // render passes that wrote it
    // A depth buffer (D-088) or colour target (D-098) read as a texture of another format: its conversion per decode
    // mode, the generation it was made from, and its swizzled views.
    struct Conversion {
        id<MTLTexture> texture = nil;
        u64 generation = 0;
        std::unordered_map<u32, id<MTLTexture>> views;
    };
    std::unordered_map<u32, Conversion> conversions;
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

struct FragmentProgramEntry {
    u32 address = 0;
    shader::FragmentProgram program;
    WriteTracker::Watch watch{};
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
    id<MTLTexture> dummy_stencil = nil;              // for Z16 buffers, which have none
    id<MTLRenderPipelineState> blit_pipeline = nil;
    id<MTLSamplerState> nearest_sampler = nil;
    id<MTLSamplerState> linear_sampler = nil;
    id<MTLTexture> dummy_texture = nil;
    id<MTLTexture> dummy_cube = nil;
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
    PassTargets pass{};
    PassTargets draw_targets{}; // targets of the draw being prepared (render-target feedback detection)
    bool pass_open = false;    // targets chosen (descriptor built)
    bool pass_started = false; // encoder created
    u32 pass_width = 0, pass_height = 0;

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
    void evict_overlapping(u32 address, u32 size, const Surface* keep);
    bool read_back(Surface& s, std::vector<u8>& raw, bool stencil = false); // raw texture bytes, tightly packed
    bool write_back(Surface& s, const char* why);     // to guest memory, in the surface's format
    bool write_back_depth(Surface& s, const char* why);
    void upload_surface_from_guest(Surface& s);
    void remove_surface(Surface* s);
    id<MTLTexture> copy_texture(id<MTLTexture> src);
    // A depth buffer as the texture of decode `mode` sees it (rsx_depth_to_texture), converted on the GPU when the
    // buffer was drawn to since; nil when it cannot be.
    Surface::Conversion* depth_as_texture(Surface& d, u32 mode);
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
// Which shader output channels land in the surface's channels (B8, G8B8 remaps).
std::array<u8, 4> surface_output_swizzle(u32 gcm_format);

} // namespace pspocket::rsx::metal
