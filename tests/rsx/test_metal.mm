// GPU tests on the Mac's Metal device (skipped without one): texture decode (Morton unswizzle, 16-bit and float
// formats, DXT in compute and as BC) against the CPU reference decoders, byte-swapped vertex and index pulling,
// MSL compilation of translated programs, and end-to-end draws from synthetic RSX command buffers (clear, triangle,
// culling, texturing, render-to-texture, readback through NV0039, flip and present).

#include <catch_amalgamated.hpp>

#import <Metal/Metal.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <random>
#include <thread>

#include "pspocket/core/log.hpp"
#include "pspocket/rsx/metal/metal_backend.hpp"
#include "pspocket/rsx/shader/msl.hpp"
#include "pspocket/rsx/texture.hpp"
#include "rsx_test_util.hpp"
#include "shader_asm.hpp"

using namespace pspocket;
using namespace pspocket::rsx;
using namespace pspocket::rsx::test;
using pspocket::rsx::metal::MetalBackend;
using pspocket::rsx::metal::MetalBackendConfig;

namespace {

bool have_metal()
{
    static const bool ok = MTLCreateSystemDefaultDevice() != nil;
    return ok;
}

#define REQUIRE_METAL()                                                                                                 \
    do {                                                                                                                \
        if (!have_metal()) SKIP("no Metal device");                                                                     \
    } while (0)

// Guest memory plus a Metal backend, without the FIFO (texture decode tests).
struct GpuFixture {
    GuestMemory mem;
    std::unique_ptr<WriteTracker> tracker;
    std::unique_ptr<MetalBackend> backend;
    static constexpr u32 kTexEa = PS_MEM_RSX_LOCAL_BASE + 0x200000;

    GpuFixture()
    {
        PS_CHECK(mem.reserve());
        PS_CHECK(mem.commit(PS_MEM_RSX_LOCAL_BASE, 0x01000000));
        tracker = std::make_unique<WriteTracker>(mem);
        backend = std::make_unique<MetalBackend>(mem, *tracker);
    }
};

std::array<int, 4> to_u8(const std::array<float, 4>& v)
{
    std::array<int, 4> r{};
    for (u32 i = 0; i < 4; i++) r[i] = static_cast<int>(std::lround(std::clamp(v[i], 0.f, 1.f) * 255.f));
    return r;
}

// Decodes level `level` of face `face` on the GPU and the CPU; returns the largest per-channel difference
// (in 8-bit steps for unorm formats, exact float otherwise).
double compare_decode(GpuFixture& g, const TextureInfo& t, u32 face, u32 level, bool force_compute, bool unorm)
{
    std::string why;
    const auto layout = compute_texture_layout(t, &why);
    REQUIRE(layout.has_value());
    const TextureLevel& l = layout->levels[face * layout->mips + level];
    std::vector<std::array<float, 4>> cpu, gpu;
    REQUIRE(decode_level_cpu(*layout, l, g.mem.host(GpuFixture::kTexEa), cpu));
    REQUIRE(g.backend->debug_decode_texture(t, GpuFixture::kTexEa, face, level, force_compute, gpu, &why));
    REQUIRE(gpu.size() == cpu.size());
    double worst = 0;
    for (usize i = 0; i < cpu.size(); i++) {
        if (unorm) {
            const auto a = to_u8(cpu[i]), b = to_u8(gpu[i]);
            for (u32 c = 0; c < 4; c++) worst = std::max(worst, static_cast<double>(std::abs(a[c] - b[c])));
        } else {
            for (u32 c = 0; c < 4; c++) worst = std::max(worst, std::abs(static_cast<double>(cpu[i][c]) - static_cast<double>(gpu[i][c])));
        }
    }
    return worst;
}

TextureInfo tex_info(u32 format, u16 w, u16 h, u32 mips, u32 pitch = 0, bool cube = false)
{
    TextureInfo t;
    t.format = format;
    t.width = w;
    t.height = h;
    t.mipmaps = mips;
    t.pitch = pitch;
    t.dimension = 2;
    t.cubemap = cube;
    t.border_type = true;
    t.location = 0;
    return t;
}

void fill_random(GuestMemory& mem, u32 ea, u32 size, u32 seed)
{
    std::mt19937 rng(seed);
    for (u32 i = 0; i < size; i++) ps_write8(mem.base(), ea + i, static_cast<u8>(rng()));
}

} // namespace

TEST_CASE("metal: device, kernels and output ring")
{
    REQUIRE_METAL();
    GpuFixture g;
    REQUIRE(g.backend->valid());
    CHECK(g.backend->device() != nullptr);
}

TEST_CASE("metal: Morton unswizzle and byte swap match the CPU reference")
{
    REQUIRE_METAL();
    GpuFixture g;
    fill_random(g.mem, GpuFixture::kTexEa, 0x40000, 1);

    // ARGB8, swizzled, full mip chain of a non-square texture
    const TextureInfo argb = tex_info(tex::A8R8G8B8, 64, 32, 7);
    for (u32 level = 0; level < 7; level++) CHECK(compare_decode(g, argb, 0, level, false, true) == 0);
    // ARGB8 linear with a padded pitch
    CHECK(compare_decode(g, tex_info(tex::A8R8G8B8 | tex::kLinearFlag, 48, 20, 1, 256), 0, 0, false, true) == 0);
    // 16-bit and 8-bit formats, swizzled
    for (u32 f : {tex::R5G6B5, tex::A1R5G5B5, tex::A4R4G4B4, tex::R5G5B5A1, tex::D1R5G5B5, tex::R6G5B5, tex::B8,
             tex::D8R8G8B8}) {
        INFO("format " << f);
        CHECK(compare_decode(g, tex_info(f, 32, 16, 2), 0, 0, false, true) == 0);
        CHECK(compare_decode(g, tex_info(f, 32, 16, 2), 0, 1, false, true) == 0);
    }
    // Cube map: face 3, level 1
    CHECK(compare_decode(g, tex_info(tex::A8R8G8B8, 16, 16, 2, 0, true), 3, 1, false, true) == 0);
    // Border texels (linear, 1 texel around the image)
    TextureInfo bordered = tex_info(tex::A8R8G8B8 | tex::kLinearFlag, 8, 8, 1, 64);
    bordered.border_type = false;
    CHECK(compare_decode(g, bordered, 0, 0, false, true) == 0);
}

TEST_CASE("metal: float texture formats decode exactly")
{
    REQUIRE_METAL();
    GpuFixture g;
    // Finite random floats/halves (random bits would include NaNs, which never compare equal).
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-1000.f, 1000.f);
    for (u32 i = 0; i < 0x8000; i += 4) ps_writef32(g.mem.base(), GpuFixture::kTexEa + i, dist(rng));
    CHECK(compare_decode(g, tex_info(tex::X32_FLOAT, 16, 16, 1), 0, 0, false, false) == 0);
    CHECK(compare_decode(g, tex_info(tex::W32_Z32_Y32_X32_FLOAT, 8, 8, 1), 0, 0, false, false) == 0);
    for (u32 i = 0; i < 0x8000; i += 2) {
        const u16 h = static_cast<u16>((rng() & 0x8000) | ((rng() % 30 + 1) << 10) | (rng() & 0x3ff));
        ps_write16(g.mem.base(), GpuFixture::kTexEa + i, h);
    }
    CHECK(compare_decode(g, tex_info(tex::W16_Z16_Y16_X16_FLOAT, 16, 8, 1), 0, 0, false, false) == 0);
    // 16-bit unsigned normalized: exact through R16Unorm and RG16Unorm.
    for (u32 i = 0; i < 0x8000; i += 2) ps_write16(g.mem.base(), GpuFixture::kTexEa + i, static_cast<u16>(rng()));
    CHECK(compare_decode(g, tex_info(tex::X16, 32, 16, 2), 0, 0, false, false) < 1e-7);
    CHECK(compare_decode(g, tex_info(tex::X16, 32, 16, 2), 0, 1, false, false) < 1e-7);
    CHECK(compare_decode(g, tex_info(tex::Y16_X16, 16, 16, 1), 0, 0, false, false) < 1e-7);
    // DEPTH16 in memory (GTA V's shadow maps outside a depth buffer the cache knows): the depth as 16-bit unorm.
    CHECK(compare_decode(g, tex_info(tex::DEPTH16, 32, 16, 2), 0, 0, false, false) < 1e-7);
    CHECK(compare_decode(g, tex_info(tex::DEPTH16, 32, 16, 2), 0, 1, false, false) < 1e-7);
}

TEST_CASE("metal: DXT1/3/5 decode in compute matches the CPU reference, BC upload agrees")
{
    REQUIRE_METAL();
    GpuFixture g;
    fill_random(g.mem, GpuFixture::kTexEa, 0x20000, 3);
    for (u32 f : {tex::COMPRESSED_DXT1, tex::COMPRESSED_DXT23, tex::COMPRESSED_DXT45}) {
        INFO("format " << f);
        const TextureInfo t = tex_info(f, 64, 32, 4);
        for (u32 level = 0; level < 4; level++) CHECK(compare_decode(g, t, 0, level, true, true) == 0);
        if (g.backend->supports_bc()) {
            // The hardware BC decoder rounds its interpolations differently: allow a few steps.
            for (u32 level = 0; level < 4; level++) CHECK(compare_decode(g, t, 0, level, false, true) <= 4);
        }
    }
}

// ---- vertex pulling --------------------------------------------------------------------------------------------

namespace {

id<MTLRenderPipelineState> fetch_pipeline(id<MTLDevice> device)
{
    const std::string src = shader::msl_prologue() + R"MSL(
vertex void rsx_test_fetch(uint vid [[vertex_id]], device const uchar* vb [[buffer(0)]],
                           constant rsx_vs_params& p [[buffer(1)]], device const uchar* ib [[buffer(3)]],
                           device float4* out [[buffer(4)]])
{
    const uint vi = rsx_vertex_index(vid, p, ib);
    for (uint a = 0; a < 8; a++) out[vid * 8 + a] = rsx_fetch(p.attribs[a], vi, vb);
}
)MSL";
    NSError* error = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()] options:nil error:&error];
    REQUIRE(lib != nil);
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"rsx_test_fetch"];
    d.rasterizationEnabled = NO;
    id<MTLRenderPipelineState> p = [device newRenderPipelineStateWithDescriptor:d error:&error];
    REQUIRE(p != nil);
    return p;
}

std::vector<std::array<float, 4>> run_fetch(id<MTLDevice> device, const std::vector<u8>& vb, const std::vector<u8>& ib,
    const shader::MslVsParams& params, u32 count)
{
    id<MTLRenderPipelineState> pso = fetch_pipeline(device);
    id<MTLBuffer> vbuf = [device newBufferWithBytes:vb.data() length:vb.size() options:MTLResourceStorageModeShared];
    id<MTLBuffer> ibuf = [device newBufferWithBytes:ib.data() length:std::max<usize>(ib.size(), 16) options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [device newBufferWithLength:count * 8 * 16 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = [device newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.renderTargetWidth = 1;
    rp.renderTargetHeight = 1;
    rp.defaultRasterSampleCount = 1;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pso];
    [enc setVertexBuffer:vbuf offset:0 atIndex:0];
    [enc setVertexBytes:&params length:sizeof(params) atIndex:1];
    [enc setVertexBuffer:ibuf offset:0 atIndex:3];
    [enc setVertexBuffer:out offset:0 atIndex:4];
    [enc drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:count];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    REQUIRE(cb.status == MTLCommandBufferStatusCompleted);
    std::vector<std::array<float, 4>> r(count * 8);
    std::memcpy(r.data(), out.contents, r.size() * 16);
    return r;
}

void put16(std::vector<u8>& b, usize at, u16 v) { ps_st16(b.data() + at, v); }
void put32(std::vector<u8>& b, usize at, u32 v) { ps_st32(b.data() + at, v); }
u16 to_half(float f)
{
    // Exact for the values used here (small integers and halves)
    u32 u;
    std::memcpy(&u, &f, 4);
    const u32 sign = (u >> 16) & 0x8000;
    const int exp = static_cast<int>((u >> 23) & 0xff) - 127 + 15;
    const u32 mant = (u >> 13) & 0x3ff;
    if ((u & 0x7fffffff) == 0) return static_cast<u16>(sign);
    return static_cast<u16>(sign | (static_cast<u32>(exp) << 10) | mant);
}

} // namespace

TEST_CASE("metal: vertex pulling decodes big-endian attributes of every type")
{
    REQUIRE_METAL();
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    // Four vertices. Layout: a0 F x3 @0 stride 12; a1 S1 x4 @48 stride 8; a2 SF x2 @80 stride 4; a3 UB x4 @96;
    // a4 S32K x2 @112; a5 CMP @128; a6 UB256 x4 @144 (strides 4); a7 register value.
    std::vector<u8> vb(160, 0);
    const s16 s1v[4][4] = {{32767, -32768, 0, 100}, {-1, 1, 16384, -16384}, {7, 8, 9, 10}, {-7, -8, -9, -10}};
    for (u32 v = 0; v < 4; v++) {
        for (u32 c = 0; c < 3; c++) {
            u32 bits;
            const float f = static_cast<float>(v * 10 + c) + 0.5f;
            std::memcpy(&bits, &f, 4);
            put32(vb, v * 12 + c * 4, bits);
        }
        for (u32 c = 0; c < 4; c++) put16(vb, 48 + v * 8 + c * 2, static_cast<u16>(s1v[v][c]));
        put16(vb, 80 + v * 4, to_half(static_cast<float>(v) + 0.5f));
        put16(vb, 82 + v * 4, to_half(-2.f * static_cast<float>(v)));
        for (u32 c = 0; c < 4; c++) vb[96 + v * 4 + c] = static_cast<u8>(v * 60 + c);
        put16(vb, 112 + v * 4, static_cast<u16>(-1000 + static_cast<int>(v)));
        put16(vb, 114 + v * 4, static_cast<u16>(1000 + v));
        // CMP: x = 1023 (max), y = -1024 (min), z = 255
        put32(vb, 128 + v * 4, (1023u) | (0x400u << 11) | (255u << 22));
        for (u32 c = 0; c < 4; c++) vb[144 + v * 4 + c] = static_cast<u8>(200 + v + c);
    }
    shader::MslVsParams p{};
    auto attr = [&](u32 i, u32 offset, u32 stride, VertexType type, u32 size) {
        p.attribs[i] = {offset, stride, static_cast<u32>(type), size, 0, shader::kAttribSwap, 0, 0, {0, 0, 0, 1}};
    };
    attr(0, 0, 12, VertexType::F, 3);
    attr(1, 48, 8, VertexType::S1, 4);
    attr(2, 80, 4, VertexType::SF, 2);
    attr(3, 96, 4, VertexType::UB, 4);
    attr(4, 112, 4, VertexType::S32K, 2);
    attr(5, 128, 4, VertexType::CMP, 1);
    attr(6, 144, 4, VertexType::UB256, 4);
    p.attribs[7] = {0, 0, 0, 0, 0, shader::kAttribRegister, 0, 0, {9.f, 8.f, 7.f, 6.f}};

    // Indexed: big-endian u16 indices {3, 0, 2} with base index 0 -> vertices 3, 0, 2
    std::vector<u8> ib(8, 0);
    put16(ib, 0, 3);
    put16(ib, 2, 0);
    put16(ib, 4, 2);
    p.index_mode = shader::kIndexU16BE;
    const auto out = run_fetch(device, vb, ib, p, 3);
    const u32 order[3] = {3, 0, 2};
    for (u32 k = 0; k < 3; k++) {
        const u32 v = order[k];
        const auto* a = &out[k * 8];
        INFO("vertex " << v);
        CHECK(a[0] == std::array<float, 4>{v * 10 + 0.5f, v * 10 + 1.5f, v * 10 + 2.5f, 1.f});
        for (u32 c = 0; c < 4; c++) CHECK(a[1][c] == Catch::Approx((s1v[v][c] + 0.5) / 32767.5).epsilon(1e-6));
        CHECK(a[2][0] == v + 0.5f);
        CHECK(a[2][1] == -2.f * v);
        CHECK(a[2][2] == 0.f);
        CHECK(a[2][3] == 1.f);
        for (u32 c = 0; c < 4; c++) CHECK(a[3][c] == Catch::Approx((v * 60 + c) / 255.0));
        CHECK(a[4][0] == -1000.f + v);
        CHECK(a[4][1] == 1000.f + v);
        CHECK(a[5][0] == Catch::Approx(1023.0 * 32 / 32767.0));
        CHECK(a[5][1] == Catch::Approx(-1024.0 * 32 / 32767.0));
        CHECK(a[5][2] == Catch::Approx(255.0 * 64 / 32767.0));
        CHECK(a[5][3] == 1.f);
        for (u32 c = 0; c < 4; c++) CHECK(a[6][c] == static_cast<float>(200 + v + c));
        CHECK(a[7] == std::array<float, 4>{9.f, 8.f, 7.f, 6.f});
    }

    // Big-endian u32 indices with a base index, and an instanced attribute (frequency 2, divide) plus a modulo one.
    std::vector<u8> ib32(16, 0);
    put32(ib32, 0, 0);
    put32(ib32, 4, 1);
    put32(ib32, 8, 2);
    shader::MslVsParams q = p;
    q.index_mode = shader::kIndexU32BE;
    q.base_index = 1; // vertices 1, 2, 3
    q.attribs[3].divisor = 2; // element = vertex / 2
    q.attribs[6].divisor = 2; // element = vertex % 2
    q.attribs[6].flags |= shader::kAttribModulo;
    const auto out2 = run_fetch(device, vb, ib32, q, 3);
    for (u32 k = 0; k < 3; k++) {
        const u32 v = k + 1;
        CHECK(out2[k * 8 + 0][0] == v * 10 + 0.5f);
        CHECK(out2[k * 8 + 3][0] == Catch::Approx(((v / 2) * 60) / 255.0));
        CHECK(out2[k * 8 + 6][0] == static_cast<float>(200 + (v % 2)));
    }
}

// ---- MSL compilation of translated programs ---------------------------------------------------------------------

TEST_CASE("metal: translated vertex and fragment programs compile")
{
    REQUIRE_METAL();
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    auto compile = [&](const shader::TranslateResult& r) {
        REQUIRE(r.ok);
        NSError* error = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:r.shader.source.c_str()]
                                                  options:nil
                                                    error:&error];
        if (!lib) FAIL(error.localizedDescription.UTF8String << "\n" << r.shader.source);
        CHECK([lib newFunctionWithName:[NSString stringWithUTF8String:r.shader.entry.c_str()]] != nil);
    };

    // Every supported VP opcode in one program.
    std::vector<VpInst> vp;
    for (u32 op : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 14u, 15u, 16u, 17u, 18u, 19u, 20u, 21u, 22u}) {
        VpInst in;
        in.vec_op = op;
        in.src[0] = vp_in();
        in.src[1] = vp_const("yzwx");
        in.src[2] = vp_temp(1);
        in.input = op % 16;
        in.constant = op;
        in.vec_result = false;
        in.vec_temp = 1 + op % 4;
        in.vec_mask = 0xf;
        vp.push_back(in);
    }
    for (u32 op : {1u, 2u, 3u, 4u, 13u, 14u, 15u, 16u}) {
        VpInst in;
        in.sca_op = op;
        in.src[2] = vp_temp(2, "wwww");
        in.vec_result = false;
        in.out = op % 16;
        in.sca_mask = mask("x");
        in.saturate = op == 2;
        vp.push_back(in);
    }
    // Condition codes: an update to CC0, then conditional writes of an output and a temp.
    VpInst cc_set;
    cc_set.vec_op = 11; // SLT
    cc_set.src[0] = vp_in();
    cc_set.src[1] = vp_const();
    cc_set.vec_result = false;
    cc_set.vec_mask = 0xf;
    cc_set.cond_update = true;
    vp.push_back(cc_set);
    VpInst cond_out;
    cond_out.vec_op = 1; // MOV
    cond_out.src[0] = vp_in();
    cond_out.out = 1;
    cond_out.vec_mask = mask("xy");
    cond_out.cond_test = true;
    cond_out.cond = 5; // ne
    cond_out.cond_sw = {3, 2, 1, 0};
    vp.push_back(cond_out);
    VpInst cond_temp;
    cond_temp.sca_op = 2; // RCP
    cond_temp.src[2] = vp_in();
    cond_temp.vec_result = true;
    cond_temp.sca_temp = 3;
    cond_temp.sca_mask = mask("w");
    cond_temp.cond_test = true;
    cond_temp.cond = 6; // ge
    vp.push_back(cond_temp);
    VpInst pos;
    pos.vec_op = 1;
    pos.src[0] = vp_temp(1);
    pos.out = 0;
    pos.vec_mask = 0xf;
    pos.end = true;
    vp.push_back(pos);
    const auto words = assemble_vp(vp);
    const auto decoded = shader::decode_vertex_program(words.data(), static_cast<u32>(words.size() / 4), 0);
    REQUIRE(decoded.has_value());
    compile(shader::translate_vertex_program(*decoded));

    // ARL, relative constants, LIT/EXP/LOG and every branch kind (characters skin through c[A0.x + n]).
    {
        std::vector<VpInst> flow;
        VpInst arl;
        arl.vec_op = 0x0d; // ARL A0, v[0]
        arl.src[0] = vp_in();
        arl.vec_mask = 0xf;
        flow.push_back(arl);
        VpInst rel;
        rel.vec_op = 1; // MOV R1, c[A0.z + 10]
        rel.src[0] = vp_const();
        rel.constant = 10;
        rel.constant_relative = true;
        rel.addr_swizzle = 2;
        rel.vec_result = false;
        rel.vec_temp = 1;
        rel.vec_mask = 0xf;
        flow.push_back(rel);
        for (u32 op : {5u, 6u, 7u}) { // EXP, LOG, LIT
            VpInst in;
            in.sca_op = op;
            in.src[2] = vp_temp(1);
            in.vec_result = false;
            in.sca_temp = 2;
            in.sca_mask = 0xf;
            flow.push_back(in);
        }
        VpInst bri; // 5: BRI L7 if CC0.x > 0
        bri.sca_op = 9;
        bri.cond = 4;
        bri.cond_test = true;
        bri.branch_target = 7;
        flow.push_back(bri);
        VpInst cal; // 6: CAL L9
        cal.sca_op = 10;
        cal.branch_target = 9;
        flow.push_back(cal);
        VpInst out; // 7: MOV o[0], R1 END
        out.vec_op = 1;
        out.src[0] = vp_temp(1);
        out.out = 0;
        out.vec_mask = 0xf;
        out.end = true;
        flow.push_back(out);
        VpInst bra; // 8: BRA (to A0.x) -- unreachable, still compiled
        bra.sca_op = 8;
        flow.push_back(bra);
        VpInst sub; // 9: MOV o[7], c[A1.x + 0] ; RET END
        sub.vec_op = 1;
        sub.src[0] = vp_const();
        sub.constant_relative = true;
        sub.addr_reg = 1;
        sub.out = 7;
        sub.vec_mask = 0xf;
        sub.sca_op = 12;
        sub.end = true;
        flow.push_back(sub);
        const auto fw = assemble_vp(flow);
        const auto fd = shader::decode_vertex_program(fw.data(), static_cast<u32>(fw.size() / 4), 0);
        REQUIRE(fd.has_value());
        CHECK(fd->instructions.size() == 10);
        compile(shader::translate_vertex_program(*fd));
    }
    shader::VpOptions points;
    points.point_size = true;
    compile(shader::translate_vertex_program(*decoded, points));

    // Every supported FP opcode.
    std::vector<FpInst> fp;
    for (u32 op : {0x01u, 0x02u, 0x03u, 0x04u, 0x38u, 0x05u, 0x06u, 0x08u, 0x09u, 0x3Au, 0x1Au, 0x1Bu, 0x1Cu, 0x1Du,
             0x23u, 0x22u, 0x10u, 0x11u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu, 0x20u, 0x21u, 0x1Fu, 0x39u, 0x15u,
             0x16u, 0x17u, 0x18u, 0x2Fu, 0x31u, 0x3Bu, 0x29u, 0x2Au}) {
        FpInst in;
        in.op = op;
        in.dst = op % 3;
        in.fp16 = (op & 1) != 0;
        in.input = 1 + op % 13;
        in.tex = op % 4;
        in.src[0] = fp_in("xyzw");
        in.src[1] = fp_const("wzyx");
        in.src[2] = fp_r(1);
        in.constant = std::array<float, 4>{1.f, 2.f, 3.f, 4.f};
        in.saturate = op == 0x02;
        in.scale = op == 0x03 ? 1 : 0;
        in.prec = op == 0x04 ? 2 : 0;
        fp.push_back(in);
    }
    FpInst kil;
    kil.op = 0x12;
    fp.push_back(kil);
    FpInst out;
    out.op = 0x01;
    out.dst = 0;
    out.src[0] = fp_r(1);
    out.end = true;
    fp.push_back(out);
    const auto bytes = assemble_fp(fp);
    const auto fdecoded = shader::decode_fragment_program(bytes.data(), static_cast<u32>(bytes.size()));
    REQUIRE(fdecoded.has_value());
    shader::FpOptions fo;
    fo.shader_control = 0x40 | 0xe; // 32-bit exports and depth export
    fo.mrt_count = 4;
    fo.texture_kind[1] = shader::TextureKind::Cube;
    fo.texture_kind[2] = shader::TextureKind::Tex1D;
    fo.texcoord_2d_mask = 0x3;
    fo.alpha_test = true;
    fo.output_swizzle[1] = {2, 1, 4, 4};
    // TXP/TXL/TXB on the cube unit are rejected; keep unit 1 2D for the compile check.
    fo.texture_kind[1] = shader::TextureKind::Tex2D;
    compile(shader::translate_fragment_program(*fdecoded, fo));
    fo.flat_colors = true;
    fo.shader_control = 0;
    fo.mrt_count = 1;
    compile(shader::translate_fragment_program(*fdecoded, fo));

    // Condition codes: RC0 set without a destination, a conditional masked write, RC1 set with one, a conditional KIL.
    FpInst set;
    set.op = 0x01;
    set.no_dest = true;
    set.set_cond = true;
    set.src[0] = fp_const();
    set.constant = std::array<float, 4>{-1.f, 0.f, 1.f, 2.f};
    FpInst cmov;
    cmov.op = 0x01;
    cmov.mask = mask("xy");
    cmov.input = 1;
    cmov.src[0] = fp_in();
    cmov.exec_lt = cmov.exec_eq = false;
    cmov.cond_sw = swz("zwxx");
    FpInst cadd;
    cadd.op = 0x03;
    cadd.dst = 1;
    cadd.fp16 = true;
    cadd.src[0] = fp_r(0);
    cadd.src[1] = fp_r(0);
    cadd.set_cond = true;
    cadd.cond_mod_reg = 1;
    cadd.exec_gt = false;
    FpInst ckil;
    ckil.op = 0x12;
    ckil.exec_eq = ckil.exec_gt = false;
    ckil.cond_reg = 1;
    FpInst cout_;
    cout_.op = 0x01;
    cout_.src[0] = fp_r(0);
    cout_.end = true;
    const auto cbytes = assemble_fp({set, cmov, cadd, ckil, cout_});
    const auto cdecoded = shader::decode_fragment_program(cbytes.data(), static_cast<u32>(cbytes.size()));
    REQUIRE(cdecoded.has_value());
    compile(shader::translate_fragment_program(*cdecoded));

    // TXL and TXB on cube maps, and a conditional RET before the last write.
    FpInst txl;
    txl.op = 0x2F;
    txl.dst = 0;
    txl.input = 4;
    txl.tex = 0;
    txl.src[0] = fp_in();
    FpInst txb;
    txb.op = 0x31;
    txb.dst = 2;
    txb.input = 5;
    txb.tex = 1;
    txb.src[0] = fp_in();
    FpInst ret;
    ret.op = 0x45;
    ret.exec_lt = ret.exec_eq = false;
    FpInst last;
    last.op = 0x01;
    last.src[0] = fp_r(2);
    last.end = true;
    const auto tbytes = assemble_fp({txl, txb, ret, last});
    const auto tdecoded = shader::decode_fragment_program(tbytes.data(), static_cast<u32>(tbytes.size()));
    REQUIRE(tdecoded.has_value());
    shader::FpOptions cube;
    cube.texture_kind[0] = shader::TextureKind::Cube;
    cube.texture_kind[1] = shader::TextureKind::Cube;
    compile(shader::translate_fragment_program(*tdecoded, cube));

    // Flow control and the pack/unpack ops (D-089): IFE with an else part, a LOOP with BRK and RET inside.
    {
        FpInst setcc;
        setcc.op = 0x01; // MOV R1, f[COL0], setting RC0
        setcc.dst = 1;
        setcc.input = 1;
        setcc.src[0] = fp_in();
        setcc.set_cond = true;
        FpInst ife;
        ife.op = 0x42;
        ife.exec_lt = false, ife.exec_eq = false;
        fp_if(ife, 0x40, 0x60);
        FpInst a;
        a.op = 0x01;
        a.src[0] = fp_const();
        a.constant = std::array<float, 4>{1.f, 0.f, 0.f, 1.f};
        FpInst b = a;
        b.constant = std::array<float, 4>{0.f, 1.f, 0.f, 1.f};
        FpInst loop;
        loop.op = 0x43;
        fp_loop(loop, 0, 4, 1, 0x100);
        std::vector<FpInst> prog{setcc, ife, a, b, loop};
        for (u32 op : {0x27u, 0x28u, 0x13u, 0x14u, 0x24u, 0x25u}) { // PKB UPB PK4 UP4 PK2 UP2 at 0x70..0xC0
            FpInst p;
            p.op = op;
            p.dst = 2;
            p.src[0] = fp_r(1);
            prog.push_back(p);
        }
        FpInst brk; // 0xD0
        brk.op = 0x40;
        brk.exec_eq = false, brk.exec_gt = false;
        prog.push_back(brk);
        FpInst fret; // 0xE0
        fret.op = 0x45;
        fret.exec_lt = false, fret.exec_gt = false;
        prog.push_back(fret);
        prog.push_back(FpInst{}); // 0xF0: NOP
        FpInst fout; // 0x100: MOV R0, R2 END
        fout.op = 0x01;
        fout.src[0] = fp_r(2);
        fout.end = true;
        prog.push_back(fout);
        const auto fbytes = assemble_fp(prog);
        const auto ffd = shader::decode_fragment_program(fbytes.data(), static_cast<u32>(fbytes.size()));
        REQUIRE(ffd.has_value());
        compile(shader::translate_fragment_program(*ffd));
    }
}

// ---- end to end -------------------------------------------------------------------------------------------------

namespace {

constexpr u32 kSurfaceOffset = 0x100000; // local memory
constexpr u32 kSurfaceB = 0x140000;
constexpr u32 kVertexOffset = 0x010000;
constexpr u32 kColorOffset = 0x011000;
constexpr u32 kUvOffset = 0x012000;
constexpr u32 kFpOffset = 0x020000;
constexpr u32 kFp2Offset = 0x021000;
constexpr u32 kTexOffset = 0x030000;
constexpr u32 kW = 64, kH = 64;

struct E2E {
    GuestMemory mem;
    std::unique_ptr<WriteTracker> tracker;
    std::unique_ptr<MetalBackend> backend;
    std::unique_ptr<Rsx> rsx;
    Config config = test_config();
    std::vector<Failure> failures;
    std::vector<metal::PresentFrame> presents;
    std::vector<std::vector<u8>> present_pixels; // BGRA8 of each presented frame
    std::mutex capture_mutex;
    std::vector<std::pair<u64, std::vector<u8>>> captures; // (flip index, BGRA8) from the capture hook
    static constexpr u32 kCmdEa = 0x00200000;
    static constexpr u32 kReadbackEa = 0x00400000; // main memory, IO 0x00100000

    explicit E2E(u32 capture_every = 0)
    {
        PS_CHECK(mem.reserve());
        PS_CHECK(mem.commit(kCmdEa, 0x100000));
        PS_CHECK(mem.commit(kReadbackEa, 0x100000));
        PS_CHECK(mem.commit(PS_MEM_RSX_LOCAL_BASE, 0x01000000));
        PS_CHECK(initialize_context_memory(mem, config.layout));
        tracker = std::make_unique<WriteTracker>(mem);
        MetalBackendConfig mc;
        mc.output_width = 128;
        mc.output_height = 128;
        mc.present = [this](const metal::PresentFrame& f) {
            presents.push_back(f);
            // Read the output back once the frame's command buffer completes.
            id<MTLTexture> t = (__bridge id<MTLTexture>)f.texture;
            id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)f.command_buffer;
            id<MTLDevice> dev = t.device;
            id<MTLBuffer> buf = [dev newBufferWithLength:t.width * t.height * 4 options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:buf destinationOffset:0
           destinationBytesPerRow:t.width * 4 destinationBytesPerImage:t.width * t.height * 4];
            [blit endEncoding];
            const usize index = present_pixels.size();
            present_pixels.emplace_back();
            auto* store = &present_pixels;
            const usize bytes = t.width * t.height * 4;
            [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
                (*store)[index].assign(static_cast<const u8*>(buf.contents), static_cast<const u8*>(buf.contents) + bytes);
            }];
        };
        mc.capture_every = capture_every;
        mc.capture = [this](const u8* bgra, u32 w, u32 h, u64 flip) {
            std::lock_guard lock(capture_mutex);
            captures.emplace_back(flip, std::vector<u8>(bgra, bgra + usize{w} * h * 4));
        };
        backend = std::make_unique<MetalBackend>(mem, *tracker, mc);
        rsx = std::make_unique<Rsx>(mem, *backend, config);
        rsx->on_failure = [this](const Failure& f) { failures.push_back(f); };
        PS_CHECK(rsx->io_map().map(0, kCmdEa, 0x100000));
        PS_CHECK(rsx->io_map().map(0x100000, kReadbackEa, 0x100000));
        rsx->set_fifo_pointers(0, 0);
    }

    CommandBuilder at(u32 offset) { return CommandBuilder(mem.base(), kCmdEa + offset, offset); }
    Rsx::RunStatus submit(const CommandBuilder& b)
    {
        ps_write32(mem.base(), config.layout.put_addr(), b.offset());
        return rsx->run_until_idle();
    }
    u32 local32(u32 offset) const { return ps_read32(mem.base(), PS_MEM_RSX_LOCAL_BASE + offset); }
    void put_local32(u32 offset, u32 v) { ps_write32(mem.base(), PS_MEM_RSX_LOCAL_BASE + offset, v); }
    void put_local_f(u32 offset, float v) { ps_writef32(mem.base(), PS_MEM_RSX_LOCAL_BASE + offset, v); }
    void put_local_bytes(u32 offset, const std::vector<u8>& bytes)
    {
        std::memcpy(mem.host(PS_MEM_RSX_LOCAL_BASE + offset), bytes.data(), bytes.size());
    }
};

// Surface A8R8G8B8 kW x kH at `offset`, no depth, viewport and scissor covering it (libgcm-style y flip).
void setup_surface(CommandBuilder& b, u32 offset)
{
    b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (2u << 5) | (1u << 8));
    b.method(m::NV4097_SET_CONTEXT_DMA_COLOR_A, loc::kDmaFrameBuffer);
    b.method(m::NV4097_SET_SURFACE_COLOR_AOFFSET, offset);
    b.method(m::NV4097_SET_SURFACE_PITCH_A, kW * 4);
    b.method(m::NV4097_SET_SURFACE_COLOR_TARGET, static_cast<u32>(SurfaceTarget::A));
    b.method(m::NV4097_SET_SURFACE_CLIP_HORIZONTAL, {kW << 16, kH << 16});
    b.method(m::NV4097_SET_VIEWPORT_HORIZONTAL, {kW << 16, kH << 16});
    b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {kW << 16, kH << 16});
    const float hw = kW / 2.f, hh = kH / 2.f;
    b.method(m::NV4097_SET_VIEWPORT_OFFSET, {f2u(hw), f2u(hh), f2u(0.5f), f2u(0.f)});
    b.method(m::NV4097_SET_VIEWPORT_SCALE, {f2u(hw), f2u(-hh), f2u(0.5f), f2u(0.f)});
}

// VP: o[0] = v[0]; o[1] = v[3]; o[7] = v[8]
void upload_vertex_program(CommandBuilder& b)
{
    VpInst p0;
    p0.vec_op = 1;
    p0.src[0] = vp_in();
    p0.input = 0;
    p0.out = 0;
    p0.vec_mask = 0xf;
    VpInst p1 = p0;
    p1.input = 3;
    p1.out = 1;
    VpInst p2 = p0;
    p2.input = 8;
    p2.out = 7;
    p2.end = true;
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM, assemble_vp({p0, p1, p2}));
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
    b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, (1u << 0) | (1u << 3) | (1u << 8));
}

void set_fragment_program(CommandBuilder& b, u32 offset, u32 control)
{
    b.method(m::NV4097_SET_SHADER_PROGRAM, offset | (loc::kLocal + 1));
    b.method(m::NV4097_SET_SHADER_CONTROL, control);
}

// Triangle (-1,-1) (1,-1) (0,1) in clip space, colour per vertex as UB4 (r, g, b, a bytes), uv as F x2.
void write_triangle(E2E& e, std::array<u32, 3> colors, bool flip_winding = false)
{
    const float xy[3][2] = {{-1.f, -1.f}, {1.f, -1.f}, {0.f, 1.f}};
    const float uv[3][2] = {{0.f, 1.f}, {1.f, 1.f}, {0.5f, 0.f}};
    for (u32 v = 0; v < 3; v++) {
        const u32 src = flip_winding ? (v == 0 ? 1 : v == 1 ? 0 : 2) : v;
        e.put_local_f(kVertexOffset + v * 16 + 0, xy[src][0]);
        e.put_local_f(kVertexOffset + v * 16 + 4, xy[src][1]);
        e.put_local_f(kVertexOffset + v * 16 + 8, 0.5f);
        e.put_local_f(kVertexOffset + v * 16 + 12, 1.f);
        e.put_local32(kColorOffset + v * 4, colors[src]); // bytes r g b a (big-endian word 0xRRGGBBAA)
        e.put_local_f(kUvOffset + v * 8 + 0, uv[src][0]);
        e.put_local_f(kUvOffset + v * 8 + 4, uv[src][1]);
    }
}

void bind_vertex_arrays(CommandBuilder& b)
{
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0, static_cast<u32>(VertexType::F) | (4u << 4) | (16u << 8));
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0, kVertexOffset);
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3, static_cast<u32>(VertexType::UB) | (4u << 4) | (4u << 8));
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3, kColorOffset);
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 8, static_cast<u32>(VertexType::F) | (2u << 4) | (8u << 8));
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 8, kUvOffset);
}

void draw_triangle(CommandBuilder& b)
{
    b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::Triangles));
    b.method(m::NV4097_DRAW_ARRAYS, (2u << 24) | 0);
    b.method(m::NV4097_SET_BEGIN_END, 0);
}

// MOV R0, f[COL0] (32-bit export) or MOV H0, f[COL0] (16-bit export)
std::vector<u8> fp_color(bool fp16)
{
    FpInst mov;
    mov.op = 0x01;
    mov.dst = 0;
    mov.fp16 = fp16;
    mov.input = 1;
    mov.src[0] = fp_in();
    mov.end = true;
    return assemble_fp({mov});
}

// R0 = TEX0(f[TEX0])
std::vector<u8> fp_texture()
{
    FpInst tex;
    tex.op = 0x17;
    tex.dst = 0;
    tex.input = 4;
    tex.tex = 0;
    tex.src[0] = fp_in();
    tex.end = true;
    return assemble_fp({tex});
}

// Pixel (x, y) of the surface read back as guest A8R8G8B8 bytes (a, r, g, b) after a flush.
std::array<u8, 4> guest_pixel(E2E& e, u32 surface_offset, u32 x, u32 y)
{
    const u8* p = e.mem.host(PS_MEM_RSX_LOCAL_BASE + surface_offset + y * kW * 4 + x * 4);
    return {p[0], p[1], p[2], p[3]};
}

} // namespace

TEST_CASE("metal e2e: an NV0039 copy of a render target is deferred, and the label after it lands after it (D-107)")
{
    // GTA V copies an 80x45 target out every frame; waiting for the GPU there cost the RSX thread 9 ms a frame. The
    // copy now lands once the GPU has the target, and what the RSX writes after it (a semaphore here) waits for it.
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff0000ff); // blue
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    b.method(m::NV0039_SET_CONTEXT_DMA_BUFFER_IN, {loc::kDmaFrameBuffer, loc::kDmaHostBuffer});
    b.method(m::NV0039_OFFSET_IN, {kSurfaceOffset, 0x100000, kW * 4, kW * 4, kW * 4, kH, 0x101});
    b.method(m::NV0039_BUFFER_NOTIFY, 0);
    b.method(m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, loc::kDmaSemaphoreRW);
    b.method(m::NV406E_SEMAPHORE_OFFSET, 65 * 0x10);
    b.method(m::NV406E_SEMAPHORE_RELEASE, 7);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle); // an idle FIFO runs what was deferred
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().deferred_readbacks == 1);
    CHECK(ps_read32(e.mem.base(), e.config.layout.label_addr(65)) == 7);
    const u8* p = e.mem.host(E2E::kReadbackEa + 48 * kW * 4 + 32 * 4);
    CHECK(std::array<u8, 4>{p[0], p[1], p[2], p[3]} == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00});
}

TEST_CASE("metal e2e: clear, upload programs, draw a triangle, read back through NV0039")
{
    REQUIRE_METAL();
    E2E e;
    REQUIRE(e.backend->valid());
    const auto fp = fp_color(false);
    e.put_local_bytes(kFpOffset, fp);
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red, opaque

    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff0000ff); // ARGB: opaque blue
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40); // 32-bit exports: R0
    bind_vertex_arrays(b);
    draw_triangle(b);
    // Label release after the draw: the guest would wait for it
    b.method(m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, loc::kDmaSemaphoreRW);
    b.method(m::NV406E_SEMAPHORE_OFFSET, 64 * 0x10);
    b.method(m::NV406E_SEMAPHORE_RELEASE, 1);
    // Copy the surface to main memory with NV0039: the RSX reads surface memory -> GPU readback first
    b.method(m::NV0039_SET_CONTEXT_DMA_BUFFER_IN, {loc::kDmaFrameBuffer, loc::kDmaHostBuffer});
    b.method(m::NV0039_OFFSET_IN, {kSurfaceOffset, 0x100000, kW * 4, kW * 4, kW * 4, kH, 0x101});
    b.method(m::NV0039_BUFFER_NOTIFY, 0);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(ps_read32(e.mem.base(), e.config.layout.label_addr(64)) == 1);

    const auto& st = e.backend->stats();
    CHECK(st.draws == 1);
    CHECK(st.draws_skipped == 0);
    CHECK(st.readbacks == 1);
    CHECK(st.load_action_clears == 1); // the clear became the pass's load action
    // Inside the triangle: red (A R G B bytes); outside: the blue clear colour
    const u8* main = e.mem.host(E2E::kReadbackEa);
    auto main_pixel = [&](u32 x, u32 y) {
        const u8* p = main + y * kW * 4 + x * 4;
        return std::array<u8, 4>{p[0], p[1], p[2], p[3]};
    };
    CHECK(main_pixel(32, 48) == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00});
    CHECK(main_pixel(32, 10) == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00}); // near the apex (top)
    CHECK(main_pixel(2, 2) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff});
    CHECK(main_pixel(60, 5) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff});
    CHECK(st.gpu_copies == 1); // copied on the GPU, then read back (D-107): the surface's own memory is not written

    std::vector<u8> rgba;
    u32 w = 0, h = 0;
    REQUIRE(e.backend->debug_read_surface(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, rgba, w, h));
    CHECK(w == kW);
    CHECK(h == kH);
    CHECK(rgba[(48 * kW + 32) * 4 + 0] == 255);
    CHECK(rgba[(48 * kW + 32) * 4 + 2] == 0);
}

TEST_CASE("metal e2e: a float16 target reads back big-endian, red first (D-080)")
{
    REQUIRE_METAL();
    E2E e;
    REQUIRE(e.backend->valid());
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(SurfaceColorFormat::F_W16Z16Y16X16) | (2u << 5) | (1u << 8));
    b.method(m::NV4097_SET_SURFACE_PITCH_A, kW * 8);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff0000ff); // ARGB: opaque blue
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    // The RSX copies the target to main memory (NV0039): the GPU copy is written back first.
    b.method(m::NV0039_SET_CONTEXT_DMA_BUFFER_IN, {loc::kDmaFrameBuffer, loc::kDmaHostBuffer});
    b.method(m::NV0039_OFFSET_IN, {kSurfaceOffset, 0x100000, kW * 8, kW * 8, kW * 8, kH, 0x101});
    b.method(m::NV0039_BUFFER_NOTIFY, 0);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().readbacks == 1);
    const u8* p = e.mem.host(E2E::kReadbackEa) + (10 * kW + 20) * 8;
    CHECK(ps_ld16(p + 0) == 0x0000); // R
    CHECK(ps_ld16(p + 2) == 0x0000); // G
    CHECK(ps_ld16(p + 4) == 0x3C00); // B = 1.0
    CHECK(ps_ld16(p + 6) == 0x3C00); // A = 1.0
}

TEST_CASE("metal e2e: 16-bit exports, vertex colour interpolation and partial clears")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(true));
    write_triangle(e, {0xff0000ff, 0x00ff00ff, 0x0000ffff}); // r, g, b corners

    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff202020);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0); // 16-bit exports: H0
    bind_vertex_arrays(b);
    draw_triangle(b);
    // Partial clear after the draw: a scissored quad (not a load action)
    b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {(8u << 16) | 0, (8u << 16) | 0});
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xffffffff);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));

    // Near each corner the colour is dominated by that vertex
    const auto bl = guest_pixel(e, kSurfaceOffset, 3, 62);
    CHECK(bl[1] > 200);
    CHECK(bl[2] < 40);
    const auto br = guest_pixel(e, kSurfaceOffset, 60, 62);
    CHECK(br[2] > 200);
    CHECK(br[1] < 40);
    const auto top = guest_pixel(e, kSurfaceOffset, 32, 3);
    CHECK(top[3] > 200);
    // Centre: a mix
    const auto mid = guest_pixel(e, kSurfaceOffset, 32, 40);
    CHECK(mid[1] > 40);
    CHECK(mid[2] > 40);
    CHECK(mid[3] > 20);
    // The scissored clear hit only the 8x8 corner
    CHECK(guest_pixel(e, kSurfaceOffset, 4, 4) == std::array<u8, 4>{0xff, 0xff, 0xff, 0xff});
    CHECK(guest_pixel(e, kSurfaceOffset, 12, 4) == std::array<u8, 4>{0xff, 0x20, 0x20, 0x20});
}

TEST_CASE("metal e2e: back-face culling follows the RSX winding")
{
    REQUIRE_METAL();
    auto run = [](u32 front_face, bool flip) {
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff}, flip);
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_CULL_FACE_ENABLE, 1);
        b.method(m::NV4097_SET_CULL_FACE, static_cast<u32>(CullFace::Back));
        b.method(m::NV4097_SET_FRONT_FACE, front_face);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        return guest_pixel(e, kSurfaceOffset, 32, 40)[1] == 0xff; // red drawn?
    };
    // The triangle (-1,-1) (1,-1) (0,1) is counter-clockwise on screen (y down, row 0 at the top).
    CHECK(run(static_cast<u32>(FrontFace::CCW), false));       // front-facing: drawn
    CHECK_FALSE(run(static_cast<u32>(FrontFace::CW), false));  // back-facing: culled
    CHECK_FALSE(run(static_cast<u32>(FrontFace::CCW), true));  // reversed vertex order: culled
    CHECK(run(static_cast<u32>(FrontFace::CW), true));
}

TEST_CASE("metal e2e: textured draw with a swizzled texture and a channel remap")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_texture());
    write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
    // 8x8 swizzled ARGB8 texture: left half green, right half blue (opaque); A R G B bytes per texel.
    for (u32 y = 0; y < 8; y++) {
        for (u32 x = 0; x < 8; x++) {
            const u32 argb = x < 4 ? 0xff00ff00 : 0xff0000ff;
            e.put_local32(kTexOffset + morton_index(x, y, 3, 3) * 4, argb);
        }
    }
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    // Texture unit 0: offset, format (local, 2D, A8R8G8B8 swizzled, 1 mip, border colour), wrap clamp, enabled,
    // remap that swaps R and B, nearest filtering, 8x8.
    b.method(m::NV4097_SET_TEXTURE_OFFSET, {
        kTexOffset,
        (loc::kLocal + 1) | (1u << 3) | (2u << 4) | (tex::A8R8G8B8 << 8) | (1u << 16),
        3u | (3u << 8) | (3u << 16),
        (1u << 31),
        0xaa6c, // R <- B, B <- R
        (1u << 16) | (1u << 24),
        (8u << 16) | 8,
        0,
    });
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(e.backend->stats().textures_decoded == 1);
    // uv.x = 0 at the bottom-left corner -> left half of the texture (green): stays green after the R/B swap.
    CHECK(guest_pixel(e, kSurfaceOffset, 8, 60) == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00});
    // Right side: texture blue, shown red after the remap.
    CHECK(guest_pixel(e, kSurfaceOffset, 56, 60) == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00});

    // Second frame, unchanged texture memory: a cache hit, no new decode.
    auto c = e.at(b.offset());
    draw_triangle(c);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    CHECK(e.backend->stats().textures_decoded == 1);
    CHECK(e.backend->stats().texture_cache_hits >= 1);
    // The CPU rewrites the texture (hash-mode write tracking): the next draw decodes again.
    for (u32 i = 0; i < 64; i++) e.put_local32(kTexOffset + i * 4, 0xffff0000);
    auto d = e.at(c.offset());
    draw_triangle(d);
    REQUIRE(e.submit(d) == Rsx::RunStatus::Idle);
    CHECK(e.backend->stats().textures_decoded == 2);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 56, 60) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff}); // red, remapped to blue
}

TEST_CASE("metal e2e: render to a surface, then sample it as a texture")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0x00ff00ff, 0x00ff00ff, 0x00ff00ff}); // green

    auto b = e.at(0);
    // Pass 1: green triangle into surface B
    setup_surface(b, kSurfaceB);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    // Pass 2: surface A, sampling surface B (linear A8R8G8B8 texture at the same address) with the same triangle
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    set_fragment_program(b, kFp2Offset, 0x40);
    b.method(m::NV4097_SET_TEXTURE_OFFSET, {
        kSurfaceB,
        (loc::kLocal + 1) | (1u << 3) | (2u << 4) | ((tex::A8R8G8B8 | tex::kLinearFlag) << 8) | (1u << 16),
        3u | (3u << 8) | (3u << 16),
        (1u << 31),
        tex::kRemapIdentity,
        (1u << 16) | (1u << 24),
        (kW << 16) | kH,
        0,
    });
    b.method(m::NV4097_SET_TEXTURE_CONTROL3, kW * 4);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().surface_samples == 1);
    CHECK(e.backend->stats().textures_decoded == 0); // the render target was bound, not decoded from memory
    CHECK(e.backend->stats().render_passes == 2);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    // uv maps the triangle onto itself: inside it reads B's green, near the bottom corners B's black clear.
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00});
}

namespace {

// Surface A at offset in another colour format (pitch in bytes), optionally with a Z24S8 depth buffer.
void setup_surface_as(CommandBuilder& b, u32 offset, SurfaceColorFormat format, u32 pitch, u32 depth_offset = 0)
{
    setup_surface(b, offset);
    b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(format) | (2u << 5) | (1u << 8));
    b.method(m::NV4097_SET_SURFACE_PITCH_A, pitch);
    if (depth_offset) {
        b.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
        b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, depth_offset);
        b.method(m::NV4097_SET_SURFACE_PITCH_Z, kW * 4);
    }
}

// Texture unit 0: a linear texture of the given format at a local memory offset, kW x kH, identity remap.
void bind_linear_texture(CommandBuilder& b, u32 offset, u32 format, u32 pitch)
{
    b.method(m::NV4097_SET_TEXTURE_OFFSET, {
        offset,
        (loc::kLocal + 1) | (1u << 3) | (2u << 4) | ((format | tex::kLinearFlag) << 8) | (1u << 16),
        3u | (3u << 8) | (3u << 16),
        (1u << 31),
        tex::kRemapIdentity,
        (1u << 16) | (1u << 24),
        (kW << 16) | kH,
        0,
    });
    b.method(m::NV4097_SET_TEXTURE_CONTROL3, pitch);
}

} // namespace

namespace {

// Surface B: black, with a green triangle; then NV0039 copies `line_length` bytes of each of its rows to kTexOffset
// (local), and surface A samples kTexOffset as a linear A8R8G8B8 texture with the same triangle. Returns where the
// commands end.
u32 copy_target_then_sample(E2E& e, u32 line_length)
{
    e.put_local_bytes(kFpOffset, fp_color(false));
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0x00ff00ff, 0x00ff00ff, 0x00ff00ff}); // green
    auto b = e.at(0);
    setup_surface(b, kSurfaceB);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    b.method(m::NV0039_SET_CONTEXT_DMA_BUFFER_IN, {loc::kDmaFrameBuffer, loc::kDmaFrameBuffer});
    b.method(m::NV0039_OFFSET_IN, {kSurfaceB, kTexOffset, kW * 4, kW * 4, line_length, kH, 0x101});
    b.method(m::NV0039_BUFFER_NOTIFY, 0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    set_fragment_program(b, kFp2Offset, 0x40);
    bind_linear_texture(b, kTexOffset, tex::A8R8G8B8, kW * 4);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    return b.offset();
}

} // namespace

TEST_CASE("metal e2e: an NV0039 copy of a render target into a texture is done on the GPU, and its draw waits for nothing (D-107)")
{
    // GTA V copies an 80x45 corner of a float target into a texture every frame and samples it: reading the target
    // back for that cost the RSX thread 9 ms a frame (run 079).
    REQUIRE_METAL();
    E2E e;
    const u32 end = copy_target_then_sample(e, kW * 4);
    const auto& st = e.backend->stats();
    CHECK(st.gpu_copies == 1);
    CHECK(st.deferred_conflicts == 0);
    CHECK(st.surface_samples == 1); // the copy's destination was sampled on the GPU
    CHECK(st.textures_decoded == 0);
    // Guest memory got the copy too, once the GPU had it.
    CHECK(ps_read32(e.mem.base(), PS_MEM_RSX_LOCAL_BASE + kTexOffset + 48 * kW * 4 + 32 * 4) == 0xff00ff00);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00}); // B's green

    // The CPU writes the destination after that: the texture over it reads guest memory, not the GPU's copy.
    for (u32 i = 0; i < kW * kH; i++) e.put_local32(kTexOffset + i * 4, 0xff0000ff); // blue
    auto c = e.at(end);
    c.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    c.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    draw_triangle(c);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    CHECK(e.backend->stats().textures_decoded == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff}); // the CPU's blue
}

TEST_CASE("metal e2e: the RSX's own semaphore release queued behind a readback satisfies its acquire at once (D-107)")
{
    // GTA V releases a semaphore after its per-frame copy and acquires it itself: waiting for the GPU there cost 9 ms
    // a frame. Its pipeline sees its own release at once; the CPU sees it land after the copy.
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0x00ff00ff, 0x00ff00ff, 0x00ff00ff});
    auto b = e.at(0);
    setup_surface(b, kSurfaceB);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    b.method(m::NV0039_SET_CONTEXT_DMA_BUFFER_IN, {loc::kDmaFrameBuffer, loc::kDmaFrameBuffer});
    b.method(m::NV0039_OFFSET_IN, {kSurfaceB, kTexOffset, kW * 4, kW * 4, kW * 4, kH, 0x101});
    b.method(m::NV0039_BUFFER_NOTIFY, 0);
    b.method(m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, loc::kDmaSemaphoreRW);
    b.method(m::NV406E_SEMAPHORE_OFFSET, 66 * 0x10);
    b.method(m::NV406E_SEMAPHORE_RELEASE, 5);
    b.method(m::NV406E_SEMAPHORE_ACQUIRE, 5);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle); // not blocked on the acquire
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().gpu_copies == 1);
    CHECK(e.backend->stats().deferred_conflicts == 0);
    CHECK(e.backend->stats().draws == 2);
    CHECK(ps_read32(e.mem.base(), e.config.layout.label_addr(66)) == 5);
}

TEST_CASE("metal e2e: a draw that samples the destination of a deferred NV0039 copy waits for the copy (D-107)")
{
    // A copy the GPU cannot do (rows of 254 bytes: not whole pixels) is deferred until the GPU has the target; a
    // texture over its destination is read after it lands, not before.
    REQUIRE_METAL();
    E2E e;
    copy_target_then_sample(e, kW * 4 - 2);
    const auto& st = e.backend->stats();
    CHECK(st.gpu_copies == 0);
    CHECK(st.deferred_readbacks == 1);
    CHECK(st.deferred_conflicts == 1); // the texture bind waited for the copy
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00}); // B's green
}

TEST_CASE("metal e2e: copies that touch nothing deferred run at once; a write over a deferred copy lands after it (D-107)")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0x00ff00ff, 0x00ff00ff, 0x00ff00ff}); // green
    // CPU data in main memory (IO 0x180000): blue pixels.
    constexpr u32 kCpuData = E2E::kReadbackEa + 0x80000;
    for (u32 i = 0; i < kW * kH; i++) ps_write32(e.mem.base(), kCpuData + i * 4, 0xff0000ff);
    auto b = e.at(0);
    setup_surface(b, kSurfaceB);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    // Surface B to main memory: deferred.
    b.method(m::NV0039_SET_CONTEXT_DMA_BUFFER_IN, {loc::kDmaFrameBuffer, loc::kDmaHostBuffer});
    b.method(m::NV0039_OFFSET_IN, {kSurfaceB, 0x100000, kW * 4, kW * 4, kW * 4, kH, 0x101});
    b.method(m::NV0039_BUFFER_NOTIFY, 0);
    // The CPU data to kTexOffset: touches nothing deferred, so it runs now, and the draw that samples it does not wait.
    b.method(m::NV0039_SET_CONTEXT_DMA_BUFFER_IN, {loc::kDmaHostBuffer, loc::kDmaFrameBuffer});
    b.method(m::NV0039_OFFSET_IN, {0x180000, kTexOffset, kW * 4, kW * 4, kW * 4, kH, 0x101});
    b.method(m::NV0039_BUFFER_NOTIFY, 0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    set_fragment_program(b, kFp2Offset, 0x40);
    bind_linear_texture(b, kTexOffset, tex::A8R8G8B8, kW * 4);
    draw_triangle(b);
    // NV308A over the first copy's destination: the copy lands first, the pixel after it.
    b.method(m::NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN, loc::kDmaHostBuffer);
    b.method(m::NV3062_SET_COLOR_FORMAT, {10, ((kW * 4) << 16) | (kW * 4), 0, 0x100000});
    b.method(m::NV308A_POINT, {0, 1, 1});
    b.method(m::NV308A_COLOR, 0x11223344);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().deferred_readbacks == 1);
    CHECK(e.backend->stats().deferred_conflicts == 1); // only the NV308A write waited
    CHECK(ps_read32(e.mem.base(), E2E::kReadbackEa) == 0x11223344);
    const u8* p = e.mem.host(E2E::kReadbackEa + 48 * kW * 4 + 32 * 4);
    CHECK(std::array<u8, 4>{p[0], p[1], p[2], p[3]} == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00}); // B's green
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff}); // the CPU's blue
}

TEST_CASE("metal e2e: draws reuse a fragment program's compile until the program is rewritten (D-109)")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red
    for (u32 i = 0; i < kW * kH; i++) e.put_local32(kTexOffset + i * 4, 0xff0000ffu); // blue texels
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    bind_linear_texture(b, kTexOffset, tex::A8R8G8B8, kW * 4);
    draw_triangle(b);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    CHECK(e.failures.empty());
    const u64 compiled = e.backend->stats().programs_compiled;
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00}); // red

    // The CPU writes another program at the same address: the next batch decodes and compiles it.
    e.put_local_bytes(kFpOffset, fp_texture());
    auto c = e.at(b.offset());
    draw_triangle(c);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().programs_compiled == compiled + 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff}); // the texture's blue
}

TEST_CASE("metal e2e: a fragment program rewritten between two draws of one batch, then set again, is decoded again (D-109)")
{
    // Setting the program again no longer decodes it again by itself, only when its bytes changed.
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red
    for (u32 i = 0; i < kW * kH; i++) e.put_local32(kTexOffset + i * 4, 0xff0000ffu); // blue texels
    const std::vector<u8> textured = fp_texture();
    REQUIRE(textured.size() % 4 == 0);
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    bind_linear_texture(b, kTexOffset, tex::A8R8G8B8, kW * 4);
    draw_triangle(b);
    // NV308A writes the textured program over it, word by word (Y32: the words as they are).
    const u32 words = static_cast<u32>(textured.size() / 4);
    b.method(m::NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN, loc::kDmaFrameBuffer);
    b.method(m::NV3062_SET_COLOR_FORMAT, {11, (0x1000u << 16) | 0x1000u, 0, kFpOffset});
    b.method(m::NV308A_POINT, {0, words, words});
    for (u32 i = 0; i < words; i++) b.method(m::NV308A_COLOR + i, ps_ld32(textured.data() + i * 4));
    set_fragment_program(b, kFpOffset, 0x40);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff}); // the texture's blue
}

namespace {

// A target whose memory the CPU rewrote reloads from it when a pass opens over it (D-110): a 1x1 clear at (0, 0)
// opens one, then the target goes back to guest memory. The bytes must come back as the CPU wrote them, but for the
// cleared pixel. `pattern` gives byte i of the surface's memory (pitch kW * bpp); `expect` what reading it back gives.
void reload_round_trip(u32 surface_format, u32 bpp, bool depth, u32 clear_mask, const std::function<u8(u32)>& pattern,
    const std::function<u8(u32, u8)>& expect)
{
    E2E e;
    const u32 pitch = kW * bpp;
    const u32 target = depth ? kSurfaceB : kSurfaceOffset;
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_SURFACE_FORMAT, surface_format);
    b.method(m::NV4097_SET_SURFACE_PITCH_A, depth ? kW * 4 : pitch);
    if (depth) {
        b.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
        b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, kSurfaceB);
        b.method(m::NV4097_SET_SURFACE_PITCH_Z, pitch);
    }
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff336699u);
    b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xffffff00u);
    b.method(m::NV4097_CLEAR_SURFACE, clear_mask); // the whole target: it exists on the GPU now
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);

    u8* const guest = e.mem.host(PS_MEM_RSX_LOCAL_BASE + target);
    for (u32 i = 0; i < pitch * kH; i++) guest[i] = pattern(i);
    e.backend->invalidate_memory(PS_MEM_RSX_LOCAL_BASE + target, pitch * kH);
    auto c = e.at(b.offset());
    c.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {(1u << 16) | 0, (1u << 16) | 0});
    c.method(m::NV4097_CLEAR_SURFACE, clear_mask);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().surface_reloads == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + target, pitch * kH));
    u32 wrong = 0, first = ~0u;
    for (u32 i = bpp; i < pitch * kH; i++) { // all but pixel (0, 0)
        if (guest[i] != expect(i, pattern(i)) && wrong++ == 0) first = i;
    }
    INFO(std::format("first wrong byte {} of {}", first, pitch * kH));
    CHECK(wrong == 0);
}

} // namespace

TEST_CASE("metal e2e: a target the CPU wrote reloads from guest memory in every format that reads back (D-110)")
{
    REQUIRE_METAL();
    const auto same = [](u32, u8 v) { return v; };
    const auto bytes = [](u32 i) { return static_cast<u8>(i * 7 + 1); };
    const auto halves = [](u32 i) { return static_cast<u8>(i % 2 == 0 ? 0x3c : i * 3); };            // 1.0 to 2.0
    const auto floats = [](u32 i) { return static_cast<u8>(i % 4 == 0 ? 0x3f : i % 4 == 1 ? 0x80 : i); }; // 1.0 to 2.0
    const u32 color = (2u << 5) | (1u << 8);
    SECTION("A8B8G8R8") { reload_round_trip(static_cast<u32>(SurfaceColorFormat::A8B8G8R8) | color, 4, false, clear::kColor, bytes, same); }
    SECTION("X8B8G8R8_O8B8G8R8: X reads back as one")
    {
        reload_round_trip(static_cast<u32>(SurfaceColorFormat::X8B8G8R8_O8B8G8R8) | color, 4, false, clear::kColor, bytes,
            [](u32 i, u8 v) { return i % 4 == 0 ? u8{0xff} : v; });
    }
    SECTION("F_W16Z16Y16X16") { reload_round_trip(static_cast<u32>(SurfaceColorFormat::F_W16Z16Y16X16) | color, 8, false, clear::kColor, halves, same); }
    SECTION("F_W32Z32Y32X32") { reload_round_trip(static_cast<u32>(SurfaceColorFormat::F_W32Z32Y32X32) | color, 16, false, clear::kColor, floats, same); }
    SECTION("F_X32") { reload_round_trip(static_cast<u32>(SurfaceColorFormat::F_X32) | color, 4, false, clear::kColor, floats, same); }
    SECTION("B8") { reload_round_trip(static_cast<u32>(SurfaceColorFormat::B8) | color, 1, false, clear::kColor, bytes, same); }
    SECTION("G8B8 (D-112)") { reload_round_trip(static_cast<u32>(SurfaceColorFormat::G8B8) | color, 2, false, clear::kColor, bytes, same); }
    SECTION("R5G6B5 (D-112)") { reload_round_trip(static_cast<u32>(SurfaceColorFormat::R5G6B5) | color, 2, false, clear::kColor, bytes, same); }
    SECTION("X1R5G5B5_O1R5G5B5: X reads back as one (D-112)")
    {
        reload_round_trip(static_cast<u32>(SurfaceColorFormat::X1R5G5B5_O1R5G5B5) | color, 2, false, clear::kColor, bytes,
            [](u32 i, u8 v) { return i % 2 == 0 ? static_cast<u8>(v | 0x80) : v; });
    }
    SECTION("X1R5G5B5_Z1R5G5B5: X reads back as zero (D-112)")
    {
        reload_round_trip(static_cast<u32>(SurfaceColorFormat::X1R5G5B5_Z1R5G5B5) | color, 2, false, clear::kColor, bytes,
            [](u32 i, u8 v) { return i % 2 == 0 ? static_cast<u8>(v & 0x7f) : v; });
    }
    SECTION("Z24S8")
    {
        reload_round_trip(static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (2u << 5) | (1u << 8), 4, true,
            clear::kDepthStencil, bytes, same);
    }
    SECTION("Z16")
    {
        reload_round_trip(static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (1u << 5) | (1u << 8), 2, true, clear::kDepth,
            bytes, same);
    }
}

TEST_CASE("metal e2e: an NV308A pixel written over a rendered target keeps what was rendered (D-110)")
{
    // The target goes to guest memory first, so the reload after the write has both.
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff0000ffu); // blue
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    b.method(m::NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN, loc::kDmaFrameBuffer);
    b.method(m::NV3062_SET_COLOR_FORMAT, {10, ((kW * 4) << 16) | (kW * 4), 0, kSurfaceOffset});
    b.method(m::NV308A_POINT, {0, 1, 1});
    b.method(m::NV308A_COLOR, 0xff00ff00u); // green at (0, 0)
    b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {(1u << 16) | 63, (1u << 16) | 63});
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor); // (63, 63): a pass over the target again
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().surface_reloads == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 0, 0) == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00});   // the NV308A pixel
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00}); // the red triangle
    CHECK(guest_pixel(e, kSurfaceOffset, 60, 5) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff});  // the blue clear
}

TEST_CASE("metal e2e: a swizzled target goes to guest memory in Morton order and comes back from it (D-111)")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red
    const u32 log2 = 6; // kW = kH = 64
    const auto at = [&](u32 x, u32 y) { return e.mem.host(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset + morton_index(x, y, log2, log2) * 4); };
    const auto pixel = [&](u32 x, u32 y) {
        const u8* p = at(x, y);
        return std::array<u8, 4>{p[0], p[1], p[2], p[3]};
    };
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    // Swizzled (type 2), 2^6 x 2^6; the pitch register is ignored for it.
    b.method(m::NV4097_SET_SURFACE_FORMAT,
        static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (2u << 5) | (2u << 8) | (log2 << 16) | (log2 << 24));
    b.method(m::NV4097_SET_SURFACE_PITCH_A, 64);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff0000ffu); // blue
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(pixel(32, 48) == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00}); // the red triangle
    CHECK(pixel(60, 5) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff});  // the blue clear

    // The CPU writes one texel; a pass over the target reloads it, Morton order and all.
    ps_write32(e.mem.base(), PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset + morton_index(10, 10, log2, log2) * 4, 0xff00ff00u);
    e.backend->invalidate_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4);
    auto c = e.at(b.offset());
    c.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {(1u << 16) | 63, (1u << 16) | 63});
    c.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().surface_reloads == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(pixel(10, 10) == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00}); // the CPU's green
    CHECK(pixel(32, 48) == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00}); // still the triangle
    CHECK(pixel(60, 5) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff});
}

TEST_CASE("metal e2e: an A8R8G8B8 texture over an A8B8G8R8 target reads its red as blue, as its bytes say")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red

    auto b = e.at(0);
    // Pass 1: red into an A8B8G8R8 target: its bytes are A B G R, so 0xff 0x00 0x00 0xff
    setup_surface_as(b, kSurfaceB, SurfaceColorFormat::A8B8G8R8, kW * 4);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    // Pass 2: those bytes read as A8R8G8B8 are blue
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    set_fragment_program(b, kFp2Offset, 0x40);
    bind_linear_texture(b, kSurfaceB, tex::A8R8G8B8, kW * 4);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().surface_samples == 1);
    CHECK(e.backend->stats().textures_decoded == 0);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff});
}

TEST_CASE("metal e2e: an A8B8G8R8 target read as a Y16_X16 texture converts on the GPU, without a readback (D-098)")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});

    auto b = e.at(0);
    // Pass 1: A 0x80, R 0x40, G 0x20, B 0x10 into an A8B8G8R8 target: its bytes are 80 10 20 40
    setup_surface_as(b, kSurfaceB, SurfaceColorFormat::A8B8G8R8, kW * 4);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0x80402010);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    // Pass 2: those bytes as Y16_X16 are 0x8010 and 0x2040, read (A, R, G, B) = (G, R, G, R) as RPCS3 maps them
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFp2Offset, 0x40);
    bind_vertex_arrays(b);
    bind_linear_texture(b, kSurfaceB, tex::Y16_X16, kW * 4);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().readbacks == 0); // run 066: a write-back and a decode from memory every frame
    CHECK(e.backend->stats().color_conversions == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0x20, 0x80, 0x20, 0x80});
}

TEST_CASE("metal e2e: a B8 target read as a B8 texture gives its byte in every channel")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0x10208040, 0x10208040, 0x10208040}); // r 0x10, g 0x20, b 0x80

    auto b = e.at(0);
    // Pass 1: a B8 target keeps the shader's blue
    setup_surface_as(b, kSurfaceB, SurfaceColorFormat::B8, kW);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    // Pass 2: read as a B8 texture
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    set_fragment_program(b, kFp2Offset, 0x40);
    bind_linear_texture(b, kSurfaceB, tex::B8, kW);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().surface_samples == 1);
    CHECK(e.backend->stats().textures_decoded == 0);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x80, 0x80, 0x80});
    // And a B8 target writes back as its bytes.
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH));
    CHECK(*e.mem.host(PS_MEM_RSX_LOCAL_BASE + kSurfaceB + 48 * kW + 32) == 0x80);
}

TEST_CASE("metal e2e: a Z24S8 depth buffer read as an A8R8G8B8 texture gives its bytes (depth high, stencil low)")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
    constexpr u32 kDepth = 0x180000;

    auto b = e.at(0);
    // Pass 1: depth 0x123456, stencil 0x78
    setup_surface_as(b, kSurfaceB, SurfaceColorFormat::A8R8G8B8, kW * 4, kDepth);
    b.method(m::NV4097_SET_STENCIL_MASK, 0xff);
    b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0x12345678);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kDepth | clear::kStencil);
    // Pass 2: the depth buffer as a texture (the lighting pass of GTA V's first mission rebuilds positions this way)
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFp2Offset, 0x40);
    bind_vertex_arrays(b);
    bind_linear_texture(b, kDepth, tex::A8R8G8B8, kW * 4);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().readbacks == 0); // converted on the GPU (D-088)
    CHECK(e.backend->stats().depth_conversions == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0x12, 0x34, 0x56, 0x78});
}

TEST_CASE("metal e2e: draws that write the stencil of the depth buffer they read convert it once (D-105)")
{
    // GTA V's lighting marks light volumes in the stencil of the depth buffer it also reads as A8R8G8B8 to rebuild
    // positions. Converting it again after every stencil write split the pass at every draw; the pass's own depth
    // buffer is read as it was at the conversion (feedback, undefined on the RSX).
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
    constexpr u32 kDepth = 0x180000;
    auto b = e.at(0);
    setup_surface_as(b, kSurfaceOffset, SurfaceColorFormat::A8R8G8B8, kW * 4, kDepth);
    b.method(m::NV4097_SET_STENCIL_MASK, 0xff);
    b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0x12345678);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kDepth | clear::kStencil);
    upload_vertex_program(b);
    set_fragment_program(b, kFp2Offset, 0x40);
    bind_vertex_arrays(b);
    bind_linear_texture(b, kDepth, tex::A8R8G8B8, kW * 4);
    // Stencil writes, no depth test: the depth stays 0x123456.
    b.method(m::NV4097_SET_STENCIL_TEST_ENABLE, 1);
    b.method(m::NV4097_SET_STENCIL_FUNC, static_cast<u32>(CompareFunc::Always));
    b.method(m::NV4097_SET_STENCIL_FUNC_REF, 0x9a);
    b.method(m::NV4097_SET_STENCIL_OP_ZPASS, static_cast<u32>(StencilOp::Replace));
    for (int i = 0; i < 3; i++) draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().depth_conversions == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0x12, 0x34, 0x56, 0x78});
}

TEST_CASE("metal e2e: a Z16 depth buffer reads as X16 (its value) and as DEPTH16 (the depth in every channel)")
{
    REQUIRE_METAL();
    for (const u32 format : {tex::X16, tex::DEPTH16}) {
        INFO(std::format("texture format {:#x}", format));
        E2E e;
        e.put_local_bytes(kFp2Offset, fp_texture());
        write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
        constexpr u32 kDepth = 0x180000;
        auto b = e.at(0);
        // Pass 1: a Z16 depth buffer cleared to 0x6000 (0.375: 96 in 8 bits, away from a rounding edge)
        setup_surface(b, kSurfaceB);
        b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (1u << 5) | (1u << 8));
        b.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
        b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, kDepth);
        b.method(m::NV4097_SET_SURFACE_PITCH_Z, kW * 2);
        b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0x6000);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kDepth);
        // Pass 2: read it
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFp2Offset, 0x40);
        bind_vertex_arrays(b);
        bind_linear_texture(b, kDepth, format, kW * 2);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) INFO(f.message);
        CHECK(e.failures.empty());
        CHECK(e.backend->stats().readbacks == 0);
        // X16 converts (D-088); DEPTH16 samples the depth buffer itself (D-106).
        CHECK(e.backend->stats().depth_conversions == (format == tex::X16 ? 1u : 0u));
        CHECK(e.backend->stats().surface_samples == (format == tex::X16 ? 0u : 1u));
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        const auto p = guest_pixel(e, kSurfaceOffset, 32, 48);
        if (format == tex::X16) {
            CHECK(p == std::array<u8, 4>{0x60, 0xff, 0x60, 0xff}); // X16 in A R G B: value, 1, value, 1 (RPCS3)
        } else {
            CHECK(p == std::array<u8, 4>{0x60, 0x60, 0x60, 0x60}); // the depth in every channel
        }
    }
}

TEST_CASE("metal e2e: a DEPTH16 texture with a compare function returns the depth comparison (shadow maps)")
{
    REQUIRE_METAL();
    // A 32x64 depth texture over the 64x64 target: depth 0.25 in its left half, 0.75 in its right; the reference (the
    // texture coordinate's z) is 0.5. LEQUAL passes where 0.5 <= depth (RPCS3 shadow_textures and TEX2D_SHADOW): the
    // right half white, the left half black; with linear filtering the 2x2 comparisons around the edge blend.
    for (const bool linear : {false, true}) {
        INFO(std::format("{} filtering", linear ? "linear" : "nearest"));
        E2E e;
        e.put_local_bytes(kFp2Offset, fp_texture());
        write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
        // Texture coordinates with z = 0.5: four floats per vertex
        const float uv[3][2] = {{0.f, 1.f}, {1.f, 1.f}, {0.5f, 0.f}};
        constexpr u32 kUv4Offset = 0x1A0000, kDepthTex = 0x1B0000, kTexW = kW / 2;
        for (u32 v = 0; v < 3; v++) {
            e.put_local_f(kUv4Offset + v * 16 + 0, uv[v][0]);
            e.put_local_f(kUv4Offset + v * 16 + 4, uv[v][1]);
            e.put_local_f(kUv4Offset + v * 16 + 8, 0.5f);
            e.put_local_f(kUv4Offset + v * 16 + 12, 1.f);
        }
        for (u32 y = 0; y < kH; y++)
            for (u32 x = 0; x < kTexW; x++) ps_st16(e.mem.host(PS_MEM_RSX_LOCAL_BASE + kDepthTex + (y * kTexW + x) * 2),
                x < kTexW / 2 ? 0x4000 : 0xC000);
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0x80808080);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFp2Offset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 8, static_cast<u32>(VertexType::F) | (4u << 4) | (16u << 8));
        b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 8, kUv4Offset);
        bind_linear_texture(b, kDepthTex, tex::DEPTH16, kTexW * 2);
        b.method(m::NV4097_SET_TEXTURE_IMAGE_RECT, (kTexW << 16) | kH);
        b.method(m::NV4097_SET_TEXTURE_ADDRESS, 3u | (3u << 8) | (3u << 16) | (3u << 28)); // clamp, zfunc LEQUAL
        b.method(m::NV4097_SET_TEXTURE_FILTER, linear ? (2u << 16) | (2u << 24) : (1u << 16) | (1u << 24));
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        CHECK(guest_pixel(e, kSurfaceOffset, 20, 48) == std::array<u8, 4>{0x00, 0x00, 0x00, 0x00});
        CHECK(guest_pixel(e, kSurfaceOffset, 44, 48) == std::array<u8, 4>{0xff, 0xff, 0xff, 0xff});
        // Pixels 31 and 32 sample texel coordinates 15.25 and 15.75: between texels 15 (fails) and 16 (passes).
        const int left = guest_pixel(e, kSurfaceOffset, 31, 48)[1], right = guest_pixel(e, kSurfaceOffset, 32, 48)[1];
        if (linear) {
            CHECK(std::abs(left - 64) <= 4);   // a quarter of texel 16
            CHECK(std::abs(right - 191) <= 4); // three quarters
        } else {
            CHECK(left == 0x00);
            CHECK(right == 0xff);
        }
    }
}

TEST_CASE("metal e2e: a shadow lookup on a depth buffer compares against the buffer itself (D-106)")
{
    // The depth buffer (Z16, 0.375) sampled directly as DEPTH16 with a compare function; the reference is the texture
    // coordinate's z, 0 here: LEQUAL passes (white), GREATER fails (black). No conversion, no readback.
    REQUIRE_METAL();
    for (const u32 zfunc : {3u, 4u}) {
        INFO(std::format("zfunc {}", zfunc));
        E2E e;
        e.put_local_bytes(kFp2Offset, fp_texture());
        write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
        constexpr u32 kDepth = 0x180000;
        auto b = e.at(0);
        setup_surface(b, kSurfaceB);
        b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (1u << 5) | (1u << 8));
        b.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
        b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, kDepth);
        b.method(m::NV4097_SET_SURFACE_PITCH_Z, kW * 2);
        b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0x6000);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kDepth);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0x80808080);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFp2Offset, 0x40);
        bind_vertex_arrays(b);
        bind_linear_texture(b, kDepth, tex::DEPTH16, kW * 2);
        b.method(m::NV4097_SET_TEXTURE_ADDRESS, 3u | (3u << 8) | (3u << 16) | (zfunc << 28));
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) INFO(f.message);
        CHECK(e.failures.empty());
        CHECK(e.backend->stats().depth_conversions == 0);
        CHECK(e.backend->stats().readbacks == 0);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        const u8 v = zfunc == 3 ? 0xff : 0x00;
        CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{v, v, v, v});
    }
}

TEST_CASE("metal e2e: depth tiles side by side in one atlas stay apart, each read as its own texture")
{
    // GTA V's shadow cascades: 128x128 Z16 tiles of one atlas, zeta offsets 0x100 apart with a 0x400 pitch. Their bytes
    // interleave row by row, but their pixels do not: each tile must survive the next one (run 070).
    REQUIRE_METAL();
    constexpr u32 kAtlas = 0x1C0000, kAtlasPitch = kW * 2 * 2; // two kW-wide Z16 tiles per row
    for (const u32 tile : {0u, 1u}) {
        INFO(std::format("tile {}", tile));
        E2E e;
        e.put_local_bytes(kFp2Offset, fp_texture());
        write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
        auto b = e.at(0);
        for (const u32 t : {0u, 1u}) {
            setup_surface(b, kSurfaceB);
            b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (1u << 5) | (1u << 8));
            b.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
            b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, kAtlas + t * kW * 2);
            b.method(m::NV4097_SET_SURFACE_PITCH_Z, kAtlasPitch);
            b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, t == 0 ? 0x4000 : 0xC000); // 0.25, then 0.75
            b.method(m::NV4097_CLEAR_SURFACE, clear::kDepth);
        }
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFp2Offset, 0x40);
        bind_vertex_arrays(b);
        bind_linear_texture(b, kAtlas + tile * kW * 2, tex::DEPTH16, kAtlasPitch);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) INFO(f.message);
        CHECK(e.failures.empty());
        CHECK(e.backend->stats().surface_samples == 1); // the tile's depth buffer, sampled (D-106), not stale memory
        CHECK(e.backend->stats().readbacks == 0);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        const u8 v = tile == 0 ? 0x40 : 0xbf;
        CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{v, v, v, v});
    }
}

namespace {

// The value word of report `index` once the GPU has written it (occlusion reports arrive from a completion handler).
u32 wait_report(E2E& e, u32 index, u32 previous)
{
    const u32 addr = e.config.layout.report_addr(index) + 8;
    for (int i = 0; i < 1000; i++) {
        const u32 v = __atomic_load_n(reinterpret_cast<const u32*>(e.mem.host(addr)), __ATOMIC_ACQUIRE);
        if (ps_bswap32(v) != previous) return ps_bswap32(v);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return previous;
}

} // namespace

TEST_CASE("metal e2e: occlusion queries count the samples that pass, and conditional rendering reads them (D-102)")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    for (u32 i = 0; i < 2; i++) ps_write32(e.mem.base(), e.config.layout.report_addr(i) + 8, 0xDEADBEEF);

    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    b.method(m::NV4097_SET_CONTEXT_DMA_REPORT, loc::kDmaReportLocal);
    // Report 0: the triangle's samples; report 1: a query with no draw.
    b.method(m::NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE, 1);
    b.method(m::NV4097_CLEAR_REPORT_VALUE, reports::kZpassPixelCount);
    draw_triangle(b);
    b.method(m::NV4097_GET_REPORT, (reports::kZpassPixelCount << 24) | 0x00);
    b.method(m::NV4097_CLEAR_REPORT_VALUE, reports::kZpassPixelCount);
    b.method(m::NV4097_GET_REPORT, (reports::kZpassPixelCount << 24) | 0x10);
    b.method(m::NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE, 0);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    e.backend->sync();
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().occlusion_reports == 2);
    // The triangle's pixels, counted from the target.
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    u32 covered = 0;
    for (u32 y = 0; y < kH; y++)
        for (u32 x = 0; x < kW; x++) covered += guest_pixel(e, kSurfaceOffset, x, y) != std::array<u8, 4>{0, 0, 0, 0};
    CHECK(covered > 1000);
    CHECK(wait_report(e, 0, 0xDEADBEEF) == covered);
    CHECK(wait_report(e, 1, 0xDEADBEEF) == 0);

    // Conditional rendering: a draw under report 1 (zero) is skipped, one under report 0 runs.
    auto c = e.at(b.offset());
    setup_surface(c, kSurfaceB);
    c.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    c.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    c.method(m::NV4097_SET_RENDER_ENABLE, (2u << 24) | 0x10);
    draw_triangle(c);
    c.method(m::NV4097_SET_RENDER_ENABLE, 1u << 24);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    CHECK(e.rsx->status().conditional_skips == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceB, 32, 48) == std::array<u8, 4>{0, 0, 0, 0});
    auto d = e.at(c.offset());
    setup_surface(d, kSurfaceB);
    d.method(m::NV4097_SET_RENDER_ENABLE, (2u << 24) | 0x00);
    draw_triangle(d);
    d.method(m::NV4097_SET_RENDER_ENABLE, 1u << 24);
    REQUIRE(e.submit(d) == Rsx::RunStatus::Idle);
    CHECK(e.rsx->status().conditional_skips == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceB, 32, 48) != std::array<u8, 4>{0, 0, 0, 0});
}

TEST_CASE("metal e2e: a swizzled 3D texture samples the slice its coordinate's z selects")
{
    // GTA V's first mission samples a 64x64x32 R5G6B5 swizzled volume. Here 4x4x4: red = 8x, green = 16y, blue = 8z.
    REQUIRE_METAL();
    for (const u32 slice : {1u, 3u}) {
        INFO(std::format("slice {}", slice));
        E2E e;
        e.put_local_bytes(kFp2Offset, fp_texture());
        write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
        const float uv[3][2] = {{0.f, 1.f}, {1.f, 1.f}, {0.5f, 0.f}};
        constexpr u32 kUv4Offset = 0x1A0000, kVolume = 0x1B0000;
        for (u32 v = 0; v < 3; v++) {
            e.put_local_f(kUv4Offset + v * 16 + 0, uv[v][0]);
            e.put_local_f(kUv4Offset + v * 16 + 4, uv[v][1]);
            e.put_local_f(kUv4Offset + v * 16 + 8, (slice + 0.5f) / 4.f);
            e.put_local_f(kUv4Offset + v * 16 + 12, 1.f);
        }
        for (u32 z = 0; z < 4; z++)
            for (u32 y = 0; y < 4; y++)
                for (u32 x = 0; x < 4; x++) {
                    u8* p = e.mem.host(PS_MEM_RSX_LOCAL_BASE + kVolume + morton_index_3d(x, y, z, 2, 2, 2) * 2);
                    ps_st16(p, static_cast<u16>(((8 * x) << 11) | ((16 * y) << 5) | (8 * z)));
                }
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFp2Offset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 8, static_cast<u32>(VertexType::F) | (4u << 4) | (16u << 8));
        b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 8, kUv4Offset);
        b.method(m::NV4097_SET_TEXTURE_OFFSET, {
            kVolume,
            (loc::kLocal + 1) | (1u << 3) | (3u << 4) | (tex::R5G6B5 << 8) | (1u << 16), // 3D, swizzled
            3u | (3u << 8) | (3u << 16),
            (1u << 31),
            tex::kRemapIdentity,
            (1u << 16) | (1u << 24), // nearest
            (4u << 16) | 4u,
            0,
        });
        b.method(m::NV4097_SET_TEXTURE_CONTROL3, 4u << 20); // depth 4
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        // Pixel (32, 48) samples texel (2, 3): red 16, green 48 (expanded as RPCS3 does), blue 8 * slice.
        const auto expand5 = [](u32 v) { return static_cast<u8>((v * 527 + 23) >> 6); };
        const auto expand6 = [](u32 v) { return static_cast<u8>((v * 259 + 33) >> 6); };
        CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, expand5(16), expand6(48), expand5(8 * slice)});
    }
}

TEST_CASE("metal e2e: a texture is checked once per batch of commands, and again after the CPU or the RSX wrote it")
{
    // D-104: a texture found clean is trusted until the CPU submits more commands, a semaphore acquire passes, or the
    // RSX itself writes guest memory. Each of those must still show new texels.
    REQUIRE_METAL();
    constexpr u32 kTex = 0x1B0000, kSource = 0x1C0000;
    const auto fill = [](E2E& e, u32 offset, u32 argb) {
        for (u32 i = 0; i < kW * kH; i++) ps_write32(e.mem.base(), PS_MEM_RSX_LOCAL_BASE + offset + i * 4, argb);
    };
    E2E e;
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
    fill(e, kTex, 0xff102030);
    fill(e, kSource, 0xff405060);
    const auto draw = [&](CommandBuilder& b) {
        setup_surface(b, kSurfaceOffset);
        upload_vertex_program(b);
        set_fragment_program(b, kFp2Offset, 0x40);
        bind_vertex_arrays(b);
        bind_linear_texture(b, kTex, tex::A8R8G8B8, kW * 4);
        draw_triangle(b);
    };
    const auto pixel = [&] {
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        return guest_pixel(e, kSurfaceOffset, 32, 48);
    };
    // Two draws in one batch: one decode, then a cache hit.
    auto b = e.at(0);
    draw(b);
    draw(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    CHECK(pixel() == std::array<u8, 4>{0xff, 0x10, 0x20, 0x30});
    CHECK(e.backend->stats().textures_decoded == 1);
    // The CPU rewrites the texture, then submits: the next batch sees it.
    fill(e, kTex, 0xff708090);
    auto c = e.at(b.offset());
    draw(c);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    CHECK(pixel() == std::array<u8, 4>{0xff, 0x70, 0x80, 0x90});
    // In one batch: a draw, an NV0039 copy over the texture, a draw: the second sees the copy.
    auto d = e.at(c.offset());
    draw(d);
    d.method(m::NV0039_SET_CONTEXT_DMA_BUFFER_IN, {loc::kDmaFrameBuffer, loc::kDmaFrameBuffer});
    d.method(m::NV0039_OFFSET_IN, {kSource, kTex, kW * 4, kW * 4, kW * 4, kH, 0x101});
    d.method(m::NV0039_BUFFER_NOTIFY, 0);
    draw(d);
    REQUIRE(e.submit(d) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(pixel() == std::array<u8, 4>{0xff, 0x40, 0x50, 0x60});
}

TEST_CASE("metal e2e: a vertex program samples a vertex texture (TXL, D-093)")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
    // A 4x4 linear A8R8G8B8 texture, every texel 0xff 0x20 0x40 0x80 (A R G B).
    for (u32 i = 0; i < 16; i++) e.put_local32(kTexOffset + i * 4, 0xff204080);

    // VP: o[0] = v[0]; o[1] = TXL(v[8].xy, TEX0) -- the colour comes from the vertex texture
    VpInst pos;
    pos.vec_op = 1;
    pos.src[0] = vp_in();
    pos.input = 0;
    pos.out = 0;
    pos.vec_mask = 0xf;
    VpInst txl;
    txl.vec_op = 0x19;
    txl.src[0] = vp_in();
    txl.src[1] = vp_temp(0); // tex_num 0
    txl.input = 8;
    txl.out = 1;
    txl.vec_mask = 0xf;
    txl.end = true;
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM, assemble_vp({pos, txl}));
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
    b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, (1u << 0) | (1u << 8));
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    b.method(m::NV4097_SET_VERTEX_TEXTURE_OFFSET, {
        kTexOffset,
        (loc::kLocal + 1) | (2u << 4) | ((tex::A8R8G8B8 | tex::kLinearFlag) << 8) | (1u << 16),
        3u | (3u << 8) | (3u << 16),
        (1u << 31),
        4u * 4,                  // CONTROL3: pitch
        (1u << 16) | (1u << 24), // nearest
        (4u << 16) | 4u,
    });
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().draws == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x20, 0x40, 0x80});
}

TEST_CASE("metal e2e: flip presents the display buffer scaled to the output size")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    e.rsx->set_display_buffer(0, {kSurfaceOffset, kW * 4, kW, kH});
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff0000ff);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    b.method(m::GCM_FLIP_COMMAND, 0);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    e.backend->sync();
    REQUIRE(e.presents.size() == 1);
    CHECK(e.presents[0].width == 128);
    CHECK(e.presents[0].height == 128);
    CHECK(e.presents[0].flip_index == 1);
    REQUIRE(e.present_pixels.size() == 1);
    const auto& px = e.present_pixels[0];
    REQUIRE(px.size() == 128 * 128 * 4);
    auto bgra = [&](u32 x, u32 y) { return std::array<u8, 4>{px[(y * 128 + x) * 4 + 0], px[(y * 128 + x) * 4 + 1], px[(y * 128 + x) * 4 + 2], px[(y * 128 + x) * 4 + 3]}; };
    CHECK(bgra(64, 96) == std::array<u8, 4>{0x00, 0x00, 0xff, 0xff}); // red in BGRA
    CHECK(bgra(4, 4) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff});   // blue
    CHECK(e.backend->stats().flips == 1);
    CHECK(e.rsx->flip_count() == 1);
}

TEST_CASE("metal e2e: every Nth flip is also copied to the CPU (capture)")
{
    REQUIRE_METAL();
    E2E e(2);
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    e.rsx->set_display_buffer(0, {kSurfaceOffset, kW * 4, kW, kH});
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    for (u32 color : {0xff00ff00u, 0xff0000ffu}) { // green, then blue
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, color);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        draw_triangle(b);
        b.method(m::GCM_FLIP_COMMAND, 0);
    }
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    e.backend->sync();
    std::lock_guard lock(e.capture_mutex);
    REQUIRE(e.captures.size() == 1); // flip 2 only
    CHECK(e.captures[0].first == 2);
    REQUIRE(e.present_pixels.size() == 2);
    CHECK(e.captures[0].second == e.present_pixels[1]);
    const auto& px = e.captures[0].second;
    CHECK(std::array<u8, 4>{px[(4 * 128 + 4) * 4], px[(4 * 128 + 4) * 4 + 1], px[(4 * 128 + 4) * 4 + 2], 0xff} ==
          std::array<u8, 4>{0xff, 0x00, 0x00, 0xff}); // the blue clear, in BGRA
}

TEST_CASE("metal e2e: unsupported shader opcodes skip the draw loudly")
{
    REQUIRE_METAL();
    E2E e;
    FpInst bad;
    bad.op = 0x2B; // BEM: not translated yet
    bad.dst = 0;
    bad.input = 1;
    bad.src[0] = fp_in();
    bad.end = true;
    e.put_local_bytes(kFpOffset, assemble_fp({bad}));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    CHECK(e.backend->stats().draws == 0);
    CHECK(e.backend->stats().draws_skipped == 1);
    REQUIRE_FALSE(e.failures.empty());
    CHECK(e.failures[0].kind == FailKind::Unsupported);
    CHECK(e.failures[0].message.find("BEM") != std::string::npos);
}
