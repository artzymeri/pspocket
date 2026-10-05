// Program and pipeline caches (plan section 7, "Shaders" and "Pipeline state"). Vertex programs come from vertex
// program memory, fragment programs from guest memory; both are hashed with embedded constants masked out
// (shader/ir.hpp), translated to MSL (shader/msl.hpp), compiled with newLibraryWithSource and cached by hash and
// translation options. Pipelines are keyed by the two programs, the attachment formats, blend state and write
// masks; depth-stencil states by their descriptor.

#include "metal_internal.hpp"

#include <format>

#include "pspocket/core/memory_map.h"
#include "pspocket/rsx/gcm.hpp"

namespace pspocket::rsx::metal {

bool PipelineKey::operator<(const PipelineKey& o) const
{
    return std::tie(vp, fp, color_formats, depth_format, stencil_format, write_masks, blend, blend_ops) <
        std::tie(o.vp, o.fp, o.color_formats, o.depth_format, o.stencil_format, o.write_masks, o.blend, o.blend_ops);
}

namespace {

MTLBlendFactor blend_factor(u32 f)
{
    switch (static_cast<BlendFactor>(f)) {
    case BlendFactor::Zero: return MTLBlendFactorZero;
    case BlendFactor::One: return MTLBlendFactorOne;
    case BlendFactor::SrcColor: return MTLBlendFactorSourceColor;
    case BlendFactor::OneMinusSrcColor: return MTLBlendFactorOneMinusSourceColor;
    case BlendFactor::SrcAlpha: return MTLBlendFactorSourceAlpha;
    case BlendFactor::OneMinusSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
    case BlendFactor::DstAlpha: return MTLBlendFactorDestinationAlpha;
    case BlendFactor::OneMinusDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
    case BlendFactor::DstColor: return MTLBlendFactorDestinationColor;
    case BlendFactor::OneMinusDstColor: return MTLBlendFactorOneMinusDestinationColor;
    case BlendFactor::SrcAlphaSaturate: return MTLBlendFactorSourceAlphaSaturated;
    case BlendFactor::ConstantColor: return MTLBlendFactorBlendColor;
    case BlendFactor::OneMinusConstantColor: return MTLBlendFactorOneMinusBlendColor;
    case BlendFactor::ConstantAlpha: return MTLBlendFactorBlendAlpha;
    case BlendFactor::OneMinusConstantAlpha: return MTLBlendFactorOneMinusBlendAlpha;
    }
    return MTLBlendFactorOne;
}

bool blend_operation(u32 e, MTLBlendOperation& op)
{
    switch (static_cast<BlendEquation>(e)) {
    case BlendEquation::Add: op = MTLBlendOperationAdd; return true;
    case BlendEquation::Min: op = MTLBlendOperationMin; return true;
    case BlendEquation::Max: op = MTLBlendOperationMax; return true;
    case BlendEquation::Subtract: op = MTLBlendOperationSubtract; return true;
    case BlendEquation::ReverseSubtract: op = MTLBlendOperationReverseSubtract; return true;
    default: op = MTLBlendOperationAdd; return false; // signed variants have no Metal equivalent
    }
}

MTLCompareFunction compare(u32 f)
{
    switch (f & 7) {
    case 0: return MTLCompareFunctionNever;
    case 1: return MTLCompareFunctionLess;
    case 2: return MTLCompareFunctionEqual;
    case 3: return MTLCompareFunctionLessEqual;
    case 4: return MTLCompareFunctionGreater;
    case 5: return MTLCompareFunctionNotEqual;
    case 6: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

MTLStencilOperation stencil_op(u32 op)
{
    switch (static_cast<StencilOp>(op)) {
    case StencilOp::Zero: return MTLStencilOperationZero;
    case StencilOp::Replace: return MTLStencilOperationReplace;
    case StencilOp::Incr: return MTLStencilOperationIncrementClamp;
    case StencilOp::Decr: return MTLStencilOperationDecrementClamp;
    case StencilOp::Invert: return MTLStencilOperationInvert;
    case StencilOp::IncrWrap: return MTLStencilOperationIncrementWrap;
    case StencilOp::DecrWrap: return MTLStencilOperationDecrementWrap;
    case StencilOp::Keep:
    default: return MTLStencilOperationKeep;
    }
}

// RSX write mask bits (R 1, G 2, B 4, A 8) -> Metal, with the B8 / G8B8 channel moves.
MTLColorWriteMask write_mask(u8 rsx, u32 gcm_format)
{
    const bool r = rsx & 1, g = rsx & 2, b = rsx & 4, a = rsx & 8;
    MTLColorWriteMask m = MTLColorWriteMaskNone;
    switch (static_cast<SurfaceColorFormat>(gcm_format)) {
    case SurfaceColorFormat::B8:
        if (b) m |= MTLColorWriteMaskRed;
        return m;
    case SurfaceColorFormat::G8B8:
        if (b) m |= MTLColorWriteMaskRed;
        if (g) m |= MTLColorWriteMaskGreen;
        return m;
    default:
        if (r) m |= MTLColorWriteMaskRed;
        if (g) m |= MTLColorWriteMaskGreen;
        if (b) m |= MTLColorWriteMaskBlue;
        if (a) m |= MTLColorWriteMaskAlpha;
        return m;
    }
}

MTLCompileOptions* shader_compile_options()
{
    MTLCompileOptions* o = [MTLCompileOptions new];
    // RSX shaders rely on IEEE behaviour (RCP(0) = inf, NaN propagation): no fast math.
    if (@available(macOS 15.0, iOS 18.0, *)) {
        o.mathMode = MTLMathModeSafe;
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        o.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    return o;
}

} // namespace

std::optional<CompiledProgram> MetalBackend::Impl::compile(const shader::TranslateResult& tr, const char* what)
{
    // One library per program: entry names only need to be unique within it.
    NSError* error = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:tr.shader.source.c_str()]
                                              options:shader_compile_options()
                                                error:&error];
    if (!lib) {
        // A translator bug, not a guest problem: log the source with the error.
        fail(std::format("MSL compilation of {} {} failed: {}", what, tr.shader.entry,
            error ? error.localizedDescription.UTF8String : "?"));
        PS_LOG_ERROR(kChan, "source:\n{}", tr.shader.source);
        return std::nullopt;
    }
    id<MTLFunction> fn = [lib newFunctionWithName:[NSString stringWithUTF8String:tr.shader.entry.c_str()]];
    if (!fn) {
        fail(std::format("MSL entry {} missing", tr.shader.entry));
        return std::nullopt;
    }
    stats.programs_compiled++;
    CompiledProgram p;
    p.function = fn;
    p.shader = tr.shader;
    return p;
}

const CompiledProgram* MetalBackend::Impl::vertex_program_for(const RsxState& state, bool points)
{
    if ((state.dirty_bits & dirty::kVertexProgram) || !vertex_program) {
        state.dirty_bits &= ~dirty::kVertexProgram;
        std::string why;
        auto vp = shader::decode_vertex_program(state.transform_program.data(), kMaxVertexProgramInstructions,
            state.transform_program_start(), &why);
        if (!vp) {
            vertex_program.reset();
            unsupported(why);
            return nullptr;
        }
        vertex_program = std::move(*vp);
    }
    if (!vertex_program) return nullptr;
    const shader::VertexProgram& vp = *vertex_program;
    std::vector<u32> ucode;
    for (const auto& in : vp.instructions) ucode.insert(ucode.end(), in.words.begin(), in.words.end());
    const std::string key = std::format("vp:{:016x}:{}", vp.hash, points ? 1 : 0);
    if (auto it = programs.find(key); it != programs.end() && it->second.ucode == ucode) return &it->second;

    shader::VpOptions options;
    options.point_size = points;
    const shader::TranslateResult tr = shader::translate_vertex_program(vp, options);
    if (!tr.ok) {
        unsupported(tr.error);
        return nullptr;
    }
    auto compiled = compile(tr, "vertex program");
    if (!compiled) return nullptr;
    compiled->ucode = std::move(ucode);
    auto [it, inserted] = programs.insert_or_assign(key, std::move(*compiled));
    (void)inserted;
    return &it->second;
}

const CompiledProgram* MetalBackend::Impl::fragment_program_for(const RsxState& state, const shader::FpOptions& options,
    const shader::FragmentProgram** program_out, u32& program_ea)
{
    const ShaderProgramInfo sp = state.shader_program();
    const auto ea = rsx->translate(sp.offset, sp.location);
    if (!ea) {
        unsupported(std::format("fragment program at offset {:#x} (location {}) is not mapped", sp.offset, sp.location));
        return nullptr;
    }
    // Readable bytes: up to 64 KB, within committed guest memory.
    u32 max_len = 0;
    const u32 page = static_cast<u32>(GuestMemory::host_page_size());
    for (u32 a = *ea & ~(page - 1); max_len < 0x10000 && mem.is_committed(a); a += page) {
        max_len = a + page - *ea;
    }
    max_len = std::min<u32>(max_len, 0x10000);
    if (sp.location == loc::kLocal) max_len = std::min(max_len, rsx->config().local_memory_size - sp.offset);
    if (max_len < 16) {
        unsupported(std::format("fragment program at {:#010x} is not in committed guest memory", *ea));
        return nullptr;
    }

    FragmentProgramEntry& entry = fragment_programs[*ea];
    // A deferred copy to the program (or its constants, read every draw) lands first (D-107). A program not seen yet:
    // its first instruction (a copy that writes a program writes its start).
    before_access(*ea, entry.program.ucode_length ? entry.program.ucode_length : 16, false);
    const u64 epoch = check_epoch();
    const bool redecode = (state.dirty_bits & dirty::kFragmentProgram) || entry.program.instructions.empty() ||
        (entry.checked != epoch && tracker.is_dirty(entry.watch));
    entry.checked = epoch;
    state.dirty_bits &= ~dirty::kFragmentProgram;
    if (redecode) {
        std::string why;
        auto fp = shader::decode_fragment_program(mem.host(*ea), max_len, &why);
        tracker.unwatch(entry.watch);
        if (!fp) {
            fragment_programs.erase(*ea);
            unsupported(std::format("fragment program at {:#010x}: {}", *ea, why));
            return nullptr;
        }
        entry.address = *ea;
        entry.program = std::move(*fp);
        entry.watch = tracker.watch(*ea, entry.program.ucode_length);
    }
    const shader::FragmentProgram& fp = entry.program;
    *program_out = &fp;
    program_ea = *ea;

    std::vector<u32> ucode;
    for (const auto& in : fp.instructions) ucode.insert(ucode.end(), in.words.begin(), in.words.end());
    std::string opt = std::format("{:x}.{}.{}.{}.{}", options.shader_control, options.mrt_count, options.texcoord_2d_mask,
        options.alpha_test ? 1 : 0, options.flat_colors ? 1 : 0);
    for (u32 i = 0; i < options.mrt_count && i < 4; i++) {
        for (u8 c : options.output_swizzle[i]) opt += static_cast<char>('0' + c);
    }
    for (u32 t = 0; t < 16; t++) {
        if (fp.texture_mask & (1u << t)) opt += std::format(".{}{}", t, static_cast<u32>(options.texture_kind[t]));
    }
    const std::string key = std::format("fp:{:016x}:{}", fp.hash, opt);
    if (auto it = programs.find(key); it != programs.end() && it->second.ucode == ucode) return &it->second;

    const shader::TranslateResult tr = shader::translate_fragment_program(fp, options);
    if (!tr.ok) {
        unsupported(tr.error);
        return nullptr;
    }
    auto compiled = compile(tr, "fragment program");
    if (!compiled) return nullptr;
    compiled->ucode = std::move(ucode);
    auto [it, inserted] = programs.insert_or_assign(key, std::move(*compiled));
    (void)inserted;
    return &it->second;
}

id<MTLRenderPipelineState> MetalBackend::Impl::pipeline_for(const CompiledProgram& vp, const CompiledProgram& fp,
    const RsxState& state)
{
    const BlendInfo blend = state.blend();
    const ColorMaskInfo masks = state.color_mask();
    PipelineKey key;
    key.vp = reinterpret_cast<uintptr_t>((__bridge void*)vp.function);
    key.fp = reinterpret_cast<uintptr_t>((__bridge void*)fp.function);
    for (u32 i = 0; i < pass.color_count; i++) {
        const Surface* s = pass.color[i];
        const u32 rsx_index = pass.rsx_index[i];
        key.color_formats[i] = static_cast<u32>(s->pixel_format);
        key.write_masks[i] = static_cast<u32>(write_mask(masks.mask[rsx_index], s->gcm_format));
        if (blend.enabled_for(rsx_index)) {
            key.blend[i] = 1u | (blend.sfactor_rgb << 1) | (blend.dfactor_rgb << 17);
            key.blend_ops[i] = blend.equation_rgb | (blend.equation_a << 16);
            key.blend[i] ^= (blend.sfactor_a * 0x9e3779b1u) ^ (blend.dfactor_a * 0x85ebca77u);
        }
    }
    if (pass.depth) {
        key.depth_format = static_cast<u32>(pass.depth->pixel_format);
        if (pass.depth->pixel_format == MTLPixelFormatDepth32Float_Stencil8) key.stencil_format = key.depth_format;
    }
    if (auto it = pipelines.find(key); it != pipelines.end()) return it->second;

    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vp.function;
    d.fragmentFunction = fp.function;
    for (u32 i = 0; i < pass.color_count; i++) {
        const u32 rsx_index = pass.rsx_index[i];
        MTLRenderPipelineColorAttachmentDescriptor* a = d.colorAttachments[i];
        a.pixelFormat = pass.color[i]->pixel_format;
        a.writeMask = static_cast<MTLColorWriteMask>(key.write_masks[i]);
        if (blend.enabled_for(rsx_index)) {
            a.blendingEnabled = YES;
            a.sourceRGBBlendFactor = blend_factor(blend.sfactor_rgb);
            a.destinationRGBBlendFactor = blend_factor(blend.dfactor_rgb);
            a.sourceAlphaBlendFactor = blend_factor(blend.sfactor_a);
            a.destinationAlphaBlendFactor = blend_factor(blend.dfactor_a);
            MTLBlendOperation op_rgb = MTLBlendOperationAdd, op_a = MTLBlendOperationAdd;
            const bool rgb_ok = blend_operation(blend.equation_rgb, op_rgb);
            const bool a_ok = blend_operation(blend.equation_a, op_a);
            if (!rgb_ok || !a_ok) {
                unsupported(std::format("blend equation {:#x}/{:#x} (signed) is not implemented", blend.equation_rgb,
                    blend.equation_a));
            }
            a.rgbBlendOperation = op_rgb;
            a.alphaBlendOperation = op_a;
        }
    }
    if (blend.logic_op_enable) unsupported("logic ops are not implemented (Metal has none)");
    if (pass.depth) {
        d.depthAttachmentPixelFormat = pass.depth->pixel_format;
        if (pass.depth->pixel_format == MTLPixelFormatDepth32Float_Stencil8) {
            d.stencilAttachmentPixelFormat = pass.depth->pixel_format;
        }
    }
    NSError* error = nil;
    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:d error:&error];
    if (!pso) {
        fail(std::format("render pipeline creation failed: {}", error ? error.localizedDescription.UTF8String : "?"));
        return nil;
    }
    stats.pipelines_created++;
    pipelines[key] = pso;
    return pso;
}

id<MTLDepthStencilState> MetalBackend::Impl::depth_state_for(const RsxState& state)
{
    // No test means no write, for depth and stencil alike (RPCS3 RSXThread.cpp get_framebuffer_layout note).
    const DepthStencilInfo ds = state.depth_stencil();
    const bool has_depth = pass.depth != nullptr;
    const bool has_stencil = has_depth && pass.depth->pixel_format == MTLPixelFormatDepth32Float_Stencil8;
    const bool depth_test = has_depth && ds.depth_test;
    const bool stencil = has_stencil && ds.stencil_test;
    u64 key = u64{depth_test} | (u64{depth_test && ds.depth_write} << 1) | (u64{ds.depth_func & 7} << 2) | (u64{stencil} << 5);
    if (stencil) {
        auto face_bits = [](const StencilFace& f) {
            return u64{f.func & 7} | (u64{f.func_mask} << 3) | (u64{f.write_mask} << 11) |
                (u64{static_cast<u32>(stencil_op(f.op_fail))} << 19) | (u64{static_cast<u32>(stencil_op(f.op_zfail))} << 22) |
                (u64{static_cast<u32>(stencil_op(f.op_zpass))} << 25);
        };
        key ^= face_bits(ds.front) << 6;
        key = key * 0x100000001b3ull ^ face_bits(ds.back);
    }
    if (auto it = depth_states.find(key); it != depth_states.end()) return it->second;
    MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = depth_test ? compare(ds.depth_func) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = depth_test && ds.depth_write;
    if (stencil) {
        auto face = [](const StencilFace& f) {
            MTLStencilDescriptor* s = [MTLStencilDescriptor new];
            s.stencilCompareFunction = compare(f.func);
            s.stencilFailureOperation = stencil_op(f.op_fail);
            s.depthFailureOperation = stencil_op(f.op_zfail);
            s.depthStencilPassOperation = stencil_op(f.op_zpass);
            s.readMask = f.func_mask;
            s.writeMask = f.write_mask;
            return s;
        };
        d.frontFaceStencil = face(ds.front);
        d.backFaceStencil = face(ds.back);
    }
    id<MTLDepthStencilState> st = [device newDepthStencilStateWithDescriptor:d];
    depth_states[key] = st;
    return st;
}

id<MTLRenderPipelineState> MetalBackend::Impl::clear_pipeline_for(u32 color_mask, bool depth, bool stencil)
{
    u64 key = u64{color_mask} | (u64{depth} << 4) | (u64{stencil} << 5) | (u64{pass.color_count} << 6);
    for (u32 i = 0; i < pass.color_count; i++) key = key * 31 + static_cast<u64>(pass.color[i]->pixel_format);
    key = key * 31 + (pass.depth ? static_cast<u64>(pass.depth->pixel_format) : 0);
    if (auto it = clear_pipelines.find(key); it != clear_pipelines.end()) return it->second;

    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [kernels newFunctionWithName:@"rsx_fullscreen_vs"];
    NSString* fs = [NSString stringWithFormat:@"rsx_clear_fs%u", pass.color_count];
    d.fragmentFunction = [kernels newFunctionWithName:fs];
    for (u32 i = 0; i < pass.color_count; i++) {
        d.colorAttachments[i].pixelFormat = pass.color[i]->pixel_format;
        MTLColorWriteMask m = MTLColorWriteMaskNone;
        if (color_mask & 1) m |= MTLColorWriteMaskRed;
        if (color_mask & 2) m |= MTLColorWriteMaskGreen;
        if (color_mask & 4) m |= MTLColorWriteMaskBlue;
        if (color_mask & 8) m |= MTLColorWriteMaskAlpha;
        d.colorAttachments[i].writeMask = m;
    }
    if (pass.depth) {
        d.depthAttachmentPixelFormat = pass.depth->pixel_format;
        if (pass.depth->pixel_format == MTLPixelFormatDepth32Float_Stencil8) d.stencilAttachmentPixelFormat = pass.depth->pixel_format;
    }
    NSError* error = nil;
    id<MTLRenderPipelineState> p = [device newRenderPipelineStateWithDescriptor:d error:&error];
    if (!p) fail(std::format("clear pipeline: {}", error ? error.localizedDescription.UTF8String : "?"));
    clear_pipelines[key] = p;
    return p;
}

} // namespace pspocket::rsx::metal
