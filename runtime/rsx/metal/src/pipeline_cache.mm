// Program and pipeline caches (plan section 7, "Shaders" and "Pipeline state"). Vertex programs come from vertex
// program memory, fragment programs from guest memory; both are hashed with embedded constants masked out
// (shader/ir.hpp), translated to MSL (shader/msl.hpp), compiled with newLibraryWithSource and cached by hash and
// translation options. Pipelines are keyed by the two programs, the attachment formats, blend state and write
// masks; depth-stencil states by their descriptor.

#include "metal_internal.hpp"

#include <chrono>

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


} // namespace

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

std::optional<CompiledProgram> MetalBackend::Impl::compile(const shader::TranslateResult& tr, const char* what)
{
    // One library per program: entry names only need to be unique within it.
    NSError* error = nil;
    const auto t0 = std::chrono::steady_clock::now();
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:tr.shader.source.c_str()]
                                              options:shader_compile_options()
                                                error:&error];
    stats.compile_ns += static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
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
    take_shader_cache(); // programs and pipelines the shader cache made since (D-114)
    take_async();        // ... and the background compiles (D-158)
    if ((state.dirty_bits & dirty::kVertexProgram) || !vertex_program_ok) {
        state.dirty_bits &= ~dirty::kVertexProgram;
        vertex_program_ok = false;
        // A program decoded lately with the same words from the same start: decoding reads only those (D-210).
        const u32 start = state.transform_program_start();
        const u32* at = state.transform_program.data() + std::min<usize>(start, kMaxVertexProgramInstructions) * 4;
        const usize room = state.transform_program.size() - static_cast<usize>(at - state.transform_program.data());
        for (usize i = 0; i < vertex_programs.size(); i++) {
            const DecodedVertexProgram& d = vertex_programs[i];
            if (d.program.start != start || d.words.size() > room || std::memcmp(at, d.words.data(), d.words.size() * 4))
                continue;
            std::rotate(vertex_programs.begin(), vertex_programs.begin() + static_cast<std::ptrdiff_t>(i),
                vertex_programs.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            vertex_program_ok = true;
            break;
        }
        if (!vertex_program_ok) {
            std::string why;
            auto vp = shader::decode_vertex_program(state.transform_program.data(), kMaxVertexProgramInstructions,
                start, &why);
            if (!vp) {
                unsupported(why);
                return nullptr;
            }
            DecodedVertexProgram d;
            for (const auto& in : vp->instructions) d.words.insert(d.words.end(), in.words.begin(), in.words.end());
            d.program = std::move(*vp);
            if (vertex_programs.size() == kDecodedVertexPrograms) vertex_programs.pop_back();
            vertex_programs.insert(vertex_programs.begin(), std::move(d));
            vertex_program_ok = true;
        }
    }
    if (!vertex_program_ok) return nullptr;
    DecodedVertexProgram& current = vertex_programs.front();
    auto& vertex_compiled = current.compiled;
    if (const CompiledProgram* c = vertex_compiled[points ? 1 : 0]) return c;
    const shader::VertexProgram& vp = current.program;
    const std::vector<u32>& ucode = current.words;
    const std::string key = std::format("vp:{:016x}:{}", vp.hash, points ? 1 : 0);
    if (auto it = programs.find(key); it != programs.end() && it->second.ucode == ucode) {
        return vertex_compiled[points ? 1 : 0] = &it->second;
    }

    shader::VpOptions options;
    options.point_size = points;
    if (async) {
        // Compiled in the background; this draw waits for it within the frame's budget, or goes without (D-158).
        if (async_failed_programs.contains(key)) return nullptr;
        if (!async->queued(key)) {
            const shader::TranslateResult tr = shader::translate_vertex_program(vp, options);
            if (!tr.ok) {
                unsupported(tr.error);
                return nullptr;
            }
            async->queue({key, tr.shader, ucode, "vertex program"});
        }
        const auto ready = [&] {
            const auto it = programs.find(key);
            return it != programs.end() && it->second.ucode == ucode;
        };
        if (!wait_async(ready)) {
            compile_pending = true;
            return nullptr;
        }
        return vertex_compiled[points ? 1 : 0] = &programs.find(key)->second;
    }
    const shader::TranslateResult tr = shader::translate_vertex_program(vp, options);
    if (!tr.ok) {
        unsupported(tr.error);
        return nullptr;
    }
    auto compiled = compile(tr, "vertex program");
    if (!compiled) return nullptr;
    compiled->ucode = ucode;
    compiled->key = key;
    auto [it, inserted] = programs.insert_or_assign(key, std::move(*compiled));
    if (shader_cache) shader_cache->record_program(it->second);
    if (!inserted) forget_program_lookups(); // the key's program changed under the lookups that hold it
    return vertex_compiled[points ? 1 : 0] = &it->second;
}

const CompiledProgram* MetalBackend::Impl::fragment_program_for(const RsxState& state, const shader::FpOptions& requested,
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

    take_shader_cache(); // D-114
    take_async();        // D-158
    FragmentProgramSlot& slot = fragment_programs[*ea];
    // A deferred copy to the program (or its constants, read every draw) lands first (D-107). A program not seen yet:
    // its first instruction (a copy that writes a program writes its start).
    before_access(*ea, slot.entry ? slot.entry->program.ucode_length : 16, false);
    const u64 epoch = check_epoch();
    // Looked up again when its instructions changed: checked once per epoch, and whenever the program is set again (a
    // game that writes a new program over the old one between draws of one batch sets it again). GTA V sets one nearly
    // every draw (D-109). The check compares its bytes with the ones it was decoded from, embedded constants left out
    // (they are read every draw): no page protection, so a game writing its constants in place before each draw takes
    // no write fault, and the RSX thread no mprotect (D-211; Assassin's Creed's crowd: 170 to 210 faults a frame).
    const auto same_code = [&](const FragmentProgramEntry& e, const std::vector<u32>& constants) {
        const u8* now = mem.host(*ea);
        u32 at = 0;
        for (const u32 c : constants) {
            if (c > at && std::memcmp(now + at, e.code.data() + at, c - at) != 0) return false;
            at = std::max(at, c + 16);
        }
        return at >= e.program.ucode_length ||
            std::memcmp(now + at, e.code.data() + at, e.program.ucode_length - at) == 0;
    };
    FragmentProgramEntry* found = slot.entry;
    if (found && found->program.ucode_length > max_len) found = nullptr;
    if (found && ((state.dirty_bits & dirty::kFragmentProgram) || slot.checked != epoch) &&
        !same_code(*found, found->program.constant_offsets))
        found = nullptr;
    slot.checked = epoch;
    state.dirty_bits &= ~dirty::kFragmentProgram;
    if (!found) {
        // Another program at this address: one decoded before, wherever it was, is that one with its compiles (D-240:
        // GTA V's programs come and go through memory it reuses, and decoding each again, and looking its compiles up,
        // was a fifth of the RSX thread's work, run 221).
        if (!shader::scan_fragment_program(mem.host(*ea), max_len, fp_scan)) {
            fragment_programs.erase(*ea);
            unsupported(std::format("fragment program at {:#010x}: no end instruction within {} bytes", *ea, max_len));
            return nullptr;
        }
        auto& same_hash = fragment_program_bodies[fp_scan.hash];
        for (const auto& e : same_hash)
            if (e->program.ucode_length == fp_scan.ucode_length && same_code(*e, fp_scan.constant_offsets)) {
                found = e.get();
                stats.fragment_programs_found_again++;
                break;
            }
        if (!found) {
            if (fragment_program_count >= kMaxFragmentPrograms) { // a bound on what is kept: all decoded again
                fragment_programs.clear();
                fragment_program_bodies.clear();
                fragment_program_count = 0;
            }
            std::string why;
            auto fp = shader::decode_fragment_program(mem.host(*ea), max_len, &why);
            if (!fp) {
                fragment_programs.erase(*ea);
                unsupported(std::format("fragment program at {:#010x}: {}", *ea, why));
                return nullptr;
            }
            auto e = std::make_unique<FragmentProgramEntry>();
            e->address = *ea;
            e->program = std::move(*fp);
            const u8* bytes = mem.host(*ea);
            e->code.assign(bytes, bytes + e->program.ucode_length);
            for (const u32 c : e->program.constant_offsets)
                if (c + 16 <= e->code.size()) std::memset(e->code.data() + c, 0, 16);
            found = e.get();
            stats.fragment_programs_decoded++;
            fragment_program_bodies[fp_scan.hash].push_back(std::move(e)); // the slot map may have been cleared
            fragment_program_count++;
        }
        fragment_programs[*ea] = {found, epoch};
    }
    FragmentProgramEntry& entry = *found;
    const shader::FragmentProgram& fp = entry.program;
    *program_out = &fp;
    program_ea = *ea;
    // The per-unit texel options (D-174) of units the program does not sample change nothing in it: without them, draws
    // that differ only there find their compile in the program's short list.
    shader::FpOptions options = requested;
    options.texture_convert_mask = static_cast<u16>(options.texture_convert_mask & fp.texture_mask);
    options.texture_alpha_kill_mask = static_cast<u16>(options.texture_alpha_kill_mask & fp.texture_mask);
    for (const auto& [o, c] : entry.compiled) {
        if (o == options) return c;
    }

    std::vector<u32> ucode;
    for (const auto& in : fp.instructions) ucode.insert(ucode.end(), in.words.begin(), in.words.end());
    std::string opt = std::format("{:x}.{}.{}.{}.{}.{}{}", options.shader_control, options.mrt_count, options.texcoord_2d_mask,
        options.alpha_test ? 1 : 0, options.flat_colors ? 1 : 0, options.srgb_output ? 1 : 0, options.alpha_to_coverage ? 1 : 0);
    for (u32 i = 0; i < options.mrt_count && i < 4; i++) {
        for (u8 c : options.output_swizzle[i]) opt += static_cast<char>('0' + c);
    }
    for (u32 t = 0; t < 16; t++) {
        if (!(fp.texture_mask & (1u << t))) continue;
        opt += std::format(".{}{}", t, static_cast<u32>(options.texture_kind[t]));
        if (options.texture_convert_mask & (1u << t)) opt += 'c'; // texel conversions (D-174)
        if (options.texture_alpha_kill_mask & (1u << t)) opt += 'k';
    }
    const std::string key = std::format("fp:{:016x}:{}", fp.hash, opt);
    const auto remember = [&](const CompiledProgram* c) {
        if (entry.compiled.size() < 8) entry.compiled.emplace_back(options, c);
        return c;
    };
    if (auto it = programs.find(key); it != programs.end() && it->second.ucode == ucode) return remember(&it->second);

    if (async) { // D-158, as for vertex programs
        if (async_failed_programs.contains(key)) return nullptr;
        if (!async->queued(key)) {
            const shader::TranslateResult tr = shader::translate_fragment_program(fp, options);
            if (!tr.ok) {
                unsupported(tr.error);
                return nullptr;
            }
            async->queue({key, tr.shader, ucode, "fragment program"});
        }
        const auto ready = [&] {
            const auto it = programs.find(key);
            return it != programs.end() && it->second.ucode == ucode;
        };
        if (!wait_async(ready)) {
            compile_pending = true;
            return nullptr;
        }
        return remember(&programs.find(key)->second);
    }
    const shader::TranslateResult tr = shader::translate_fragment_program(fp, options);
    if (!tr.ok) {
        unsupported(tr.error);
        return nullptr;
    }
    auto compiled = compile(tr, "fragment program");
    if (!compiled) return nullptr;
    compiled->ucode = std::move(ucode);
    compiled->key = key;
    auto [it, inserted] = programs.insert_or_assign(key, std::move(*compiled));
    if (shader_cache) shader_cache->record_program(it->second);
    if (!inserted) forget_program_lookups(); // the key's program changed under the lookups that hold it
    return remember(&it->second);
}

void MetalBackend::Impl::forget_program_lookups()
{
    for (auto& [hash, list] : fragment_program_bodies)
        for (auto& e : list) e->compiled.clear();
    for (auto& d : vertex_programs) d.compiled = {};
}

PipelineKey pipeline_key(const PipelineParams& p, id<MTLFunction> vp, id<MTLFunction> fp)
{
    PipelineKey key;
    key.vp = reinterpret_cast<uintptr_t>((__bridge void*)vp);
    key.fp = reinterpret_cast<uintptr_t>((__bridge void*)fp);
    for (u32 i = 0; i < p.color_count; i++) {
        key.color_formats[i] = p.color_formats[i];
        key.write_masks[i] = p.write_masks[i];
        if (p.blend_on[i]) {
            key.blend[i] = 1u | (p.sfactor_rgb << 1) | (p.dfactor_rgb << 17);
            key.blend_ops[i] = p.equation_rgb | (p.equation_a << 16);
            key.blend[i] ^= (p.sfactor_a * 0x9e3779b1u) ^ (p.dfactor_a * 0x85ebca77u);
        }
    }
    key.depth_format = p.depth_format;
    key.stencil_format = p.stencil_format;
    return key;
}

MTLRenderPipelineDescriptor* pipeline_descriptor(const PipelineParams& p, id<MTLFunction> vp, id<MTLFunction> fp)
{
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vp;
    d.fragmentFunction = fp;
    for (u32 i = 0; i < p.color_count; i++) {
        MTLRenderPipelineColorAttachmentDescriptor* a = d.colorAttachments[i];
        a.pixelFormat = static_cast<MTLPixelFormat>(p.color_formats[i]);
        a.writeMask = static_cast<MTLColorWriteMask>(p.write_masks[i]);
        if (p.blend_on[i]) {
            a.blendingEnabled = YES;
            a.sourceRGBBlendFactor = blend_factor(p.sfactor_rgb);
            a.destinationRGBBlendFactor = blend_factor(p.dfactor_rgb);
            a.sourceAlphaBlendFactor = blend_factor(p.sfactor_a);
            a.destinationAlphaBlendFactor = blend_factor(p.dfactor_a);
            MTLBlendOperation op_rgb = MTLBlendOperationAdd, op_a = MTLBlendOperationAdd;
            blend_operation(p.equation_rgb, op_rgb);
            blend_operation(p.equation_a, op_a);
            a.rgbBlendOperation = op_rgb;
            a.alphaBlendOperation = op_a;
        }
    }
    if (p.depth_format) d.depthAttachmentPixelFormat = static_cast<MTLPixelFormat>(p.depth_format);
    if (p.stencil_format) d.stencilAttachmentPixelFormat = static_cast<MTLPixelFormat>(p.stencil_format);
    return d;
}

id<MTLRenderPipelineState> MetalBackend::Impl::pipeline_for(const CompiledProgram& vp, const CompiledProgram& fp,
    const RsxState& state)
{
    const BlendInfo blend = state.blend();
    const ColorMaskInfo masks = state.color_mask();
    PipelineParams p;
    p.color_count = pass.color_count;
    for (u32 i = 0; i < pass.color_count; i++) {
        const Surface* s = pass.color[i];
        const u32 rsx_index = pass.rsx_index[i];
        p.color_formats[i] = static_cast<u32>(s->pixel_format);
        p.write_masks[i] = static_cast<u32>(write_mask(masks.mask[rsx_index], s->gcm_format));
        p.blend_on[i] = blend.enabled_for(rsx_index) ? 1 : 0;
    }
    p.sfactor_rgb = blend.sfactor_rgb, p.dfactor_rgb = blend.dfactor_rgb;
    p.sfactor_a = blend.sfactor_a, p.dfactor_a = blend.dfactor_a;
    p.equation_rgb = blend.equation_rgb, p.equation_a = blend.equation_a;
    if (pass.depth) {
        p.depth_format = static_cast<u32>(pass.depth->pixel_format);
        if (pass.depth->pixel_format == MTLPixelFormatDepth32Float_Stencil8) p.stencil_format = p.depth_format;
    }
    take_shader_cache(); // pipelines the shader cache made since (D-114)
    take_async();        // ... and the background compiles (D-158)
    const PipelineKey key = pipeline_key(p, vp.function, fp.function);
    if (auto it = pipelines.find(key); it != pipelines.end()) return it->second;
    const auto made = [&] { return pipelines.contains(key); };
    if (async && async->queued(key)) {
        if (!wait_async(made)) {
            compile_pending = true;
            return nil;
        }
        return pipelines.at(key);
    }
    if (async && async_failed_pipelines.contains(key)) return nil;

    bool blends = false;
    for (u32 i = 0; i < p.color_count; i++) blends |= p.blend_on[i] != 0;
    MTLBlendOperation op = MTLBlendOperationAdd;
    if (blends && (!blend_operation(p.equation_rgb, op) || !blend_operation(p.equation_a, op))) {
        unsupported(std::format("blend equation {:#x}/{:#x} (signed) is not implemented", blend.equation_rgb,
            blend.equation_a));
    }
    if (blend.logic_op_enable) unsupported("logic ops are not implemented (Metal has none)");
    MTLRenderPipelineDescriptor* d = pipeline_descriptor(p, vp.function, fp.function);
    // The GPU binaries earlier runs kept (D-143): a pipeline the cache has not made yet may be there.
    if (shader_cache) {
        if (NSArray<id<MTLBinaryArchive>>* archives = shader_cache->archives(); archives.count) d.binaryArchives = archives;
    }
    if (async) {
        async->queue({key, d, vp.key, fp.key, p});
        if (!wait_async(made)) {
            compile_pending = true;
            return nil;
        }
        return pipelines.at(key);
    }
    NSError* error = nil;
    const auto t0 = std::chrono::steady_clock::now();
    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:d error:&error];
    stats.pipeline_ns += static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    if (!pso) {
        fail(std::format("render pipeline creation failed: {}", error ? error.localizedDescription.UTF8String : "?"));
        return nil;
    }
    stats.pipelines_created++;
    pipelines[key] = pso;
    if (shader_cache) shader_cache->record_pipeline(vp.key, fp.key, p);
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
