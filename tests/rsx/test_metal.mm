// GPU tests on the Mac's Metal device (skipped without one): texture decode (Morton unswizzle, 16-bit and float
// formats, DXT in compute and as BC) against the CPU reference decoders, byte-swapped vertex and index pulling,
// MSL compilation of translated programs, and end-to-end draws from synthetic RSX command buffers (clear, triangle,
// culling, texturing, render-to-texture, readback through NV0039, flip and present).

#include <catch_amalgamated.hpp>
#include <fstream>

#import <Metal/Metal.h>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <unistd.h>

#include "pspocket/core/log.hpp"
#include "pspocket/rsx/capture.hpp"
#include "pspocket/rsx/metal/metal_backend.hpp"
#include "pspocket/rsx/shader/msl.hpp"
#include "pspocket/rsx/shader/interp.hpp"
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
             tex::D8R8G8B8, tex::G8B8}) {
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
    // 24-bit depth in memory (D-163): unorm, and the 24-bit float (exponents below 0xFF: no infinities or NaNs).
    for (u32 i = 0; i < 0x8000; i += 4) ps_write32(g.mem.base(), GpuFixture::kTexEa + i, static_cast<u32>(rng()));
    CHECK(compare_decode(g, tex_info(tex::DEPTH24_D8, 16, 16, 1), 0, 0, false, false) < 1e-6);
    for (u32 i = 0; i < 0x8000; i += 4)
        ps_write32(g.mem.base(), GpuFixture::kTexEa + i, (static_cast<u32>(rng()) & 0x00FFFFFFu) | ((rng() % 0xFE) << 24));
    CHECK(compare_decode(g, tex_info(tex::DEPTH24_D8_FLOAT, 16, 16, 1), 0, 0, false, false) == 0);
    // HILO8 and HILO_S8 (D-163): two bytes each, unsigned and signed normalized.
    for (u32 i = 0; i < 0x8000; i++) ps_write8(g.mem.base(), GpuFixture::kTexEa + i, static_cast<u8>(rng()));
    CHECK(compare_decode(g, tex_info(tex::COMPRESSED_HILO8, 32, 16, 1), 0, 0, false, false) < 1e-6);
    CHECK(compare_decode(g, tex_info(tex::COMPRESSED_HILO_S8, 32, 16, 1), 0, 0, false, false) < 1e-3); // half storage
    for (u32 i = 0; i < 0x8000; i += 2) {
        const u16 h = static_cast<u16>((rng() & 0x8000) | ((rng() % 30 + 1) << 10) | (rng() & 0x3ff));
        ps_write16(g.mem.base(), GpuFixture::kTexEa + i, h);
    }
    CHECK(compare_decode(g, tex_info(tex::Y16_X16_FLOAT, 16, 16, 1), 0, 0, false, false) == 0);
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

    // Every supported FP opcode (D-174 added LIT 0x1E, LIF 0x3C, DST 0x07, REFL 0x36, DP2A 0x2E, PKG 0x2C, UPG 0x2D,
    // BEM 0x2B, TEXBEM 0x33 and TXPBEM 0x34).
    std::vector<FpInst> fp;
    for (u32 op : {0x01u, 0x02u, 0x03u, 0x04u, 0x38u, 0x05u, 0x06u, 0x08u, 0x09u, 0x3Au, 0x1Au, 0x1Bu, 0x1Cu, 0x1Du,
             0x23u, 0x22u, 0x10u, 0x11u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu, 0x20u, 0x21u, 0x1Fu, 0x39u, 0x15u,
             0x16u, 0x17u, 0x18u, 0x19u, 0x2Fu, 0x31u, 0x3Bu, 0x29u, 0x2Au, 0x1Eu, 0x3Cu, 0x07u, 0x36u, 0x2Eu, 0x2Cu,
             0x2Du, 0x2Bu, 0x33u, 0x34u}) {
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
        in.texture_expand = (op & 2) != 0; // _bx2 on half the texture ops (D-174)
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
    compile(shader::translate_fragment_program(*fdecoded, fo));
    fo.flat_colors = true;
    fo.shader_control = 0;
    fo.mrt_count = 1;
    compile(shader::translate_fragment_program(*fdecoded, fo));
    // Every texture op on every kind of texture (D-174: TXP on cube maps and TXD on shadow maps were refused).
    for (const auto kind : {shader::TextureKind::Tex2D, shader::TextureKind::Cube, shader::TextureKind::Tex1D,
             shader::TextureKind::Shadow2D, shader::TextureKind::Tex3D, shader::TextureKind::Depth2D,
             shader::TextureKind::DepthShadow2D}) {
        INFO(std::format("texture kind {}", static_cast<u32>(kind)));
        shader::FpOptions k;
        k.texture_kind.fill(kind);
        compile(shader::translate_fragment_program(*fdecoded, k));
        k.texture_convert_mask = k.texture_alpha_kill_mask = 0xffff; // and with every texel conversion (D-174)
        compile(shader::translate_fragment_program(*fdecoded, k));
    }

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
        fp_loop(loop, 4, 0, 1, 0x100);
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
constexpr u32 kFp3Offset = 0x022000;
constexpr u32 kTexOffset = 0x030000;
constexpr u32 kW = 64, kH = 64;

// What a test changes in the backend configuration of the E2E fixtures it makes (the pass bench, D-945).
std::function<void(MetalBackendConfig&)> g_e2e_config;

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
    // The present hook's read-backs: each frame's completion handler fills its place under present_mutex (a later
    // frame's emplace_back may move the vector meanwhile), and ~E2E waits for those still to run, so none writes into
    // the fixture after it is gone (D-1054: the pixels of a finished test's frame landed in the next test's heap).
    std::mutex present_mutex;
    std::atomic<u32> presents_pending{0};
    std::vector<std::pair<u64, std::vector<u8>>> captures; // (flip index, BGRA8) from the capture hook
    static constexpr u32 kCmdEa = 0x00200000;
    static constexpr u32 kReadbackEa = 0x00400000; // main memory, IO 0x00100000

    explicit E2E(u32 capture_every = 0, std::string shader_cache = {}, u32 evict_after_flips = 1800,
        bool overlay_targets = true, double async_wait_ms = -1, bool shader_interpreter = true,
        bool force_interpreter = false)
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
            usize index = 0;
            {
                std::lock_guard lock(present_mutex);
                index = present_pixels.size();
                present_pixels.emplace_back();
            }
            presents_pending.fetch_add(1, std::memory_order_acq_rel);
            E2E* self = this;
            const usize bytes = t.width * t.height * 4;
            [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
                {
                    std::lock_guard lock(self->present_mutex);
                    self->present_pixels[index].assign(static_cast<const u8*>(buf.contents),
                        static_cast<const u8*>(buf.contents) + bytes);
                }
                self->presents_pending.fetch_sub(1, std::memory_order_acq_rel); // the last touch of the fixture
            }];
        };
        mc.capture_every = capture_every;
        mc.shader_cache = std::move(shader_cache);
        mc.shader_cache_threads = 2;
        mc.evict_after_flips = evict_after_flips;
        mc.overlay_targets = overlay_targets;
        mc.async_shaders = async_wait_ms >= 0;
        if (async_wait_ms >= 0) mc.compile_wait_ms = async_wait_ms;
        mc.shader_interpreter = shader_interpreter;
        mc.force_interpreter = force_interpreter;
        mc.capture = [this](const u8* bgra, u32 w, u32 h, u64 flip) {
            std::lock_guard lock(capture_mutex);
            captures.emplace_back(flip, std::vector<u8>(bgra, bgra + usize{w} * h * 4));
        };
        if (g_e2e_config) g_e2e_config(mc);
        backend = std::make_unique<MetalBackend>(mem, *tracker, mc);
        rsx = std::make_unique<Rsx>(mem, *backend, config);
        rsx->on_failure = [this](const Failure& f) { failures.push_back(f); };
        PS_CHECK(rsx->io_map().map(0, kCmdEa, 0x100000));
        PS_CHECK(rsx->io_map().map(0x100000, kReadbackEa, 0x100000));
        rsx->set_fifo_pointers(0, 0);
    }
    ~E2E()
    {
        if (backend) backend->sync();
        while (presents_pending.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    E2E(const E2E&) = delete;
    E2E& operator=(const E2E&) = delete;

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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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

namespace {

// The triangle drawn with a vertex program that writes o[1] (COL0) = c[0], o[3] (BFC0) = c[1] and o[5] (FOGC) = c[2],
// a fragment program that moves fragment input `fp_input` to R0, and `state` set before the draw. The centre pixel
// as guest bytes (a, r, g, b).
std::array<u8, 4> draw_with_outputs(const std::array<std::array<float, 4>, 3>& c, u32 fp_input, bool flip,
    const std::function<void(CommandBuilder&)>& state)
{
    E2E e;
    FpInst mov;
    mov.op = 0x01;
    mov.dst = 0;
    mov.input = fp_input;
    mov.src[0] = fp_in();
    mov.end = true;
    e.put_local_bytes(kFpOffset, assemble_fp({mov}));
    write_triangle(e, {0, 0, 0}, flip);
    const auto vp_mov = [](u32 out, VpSrc src, u32 constant = 0) {
        VpInst in;
        in.vec_op = 1;
        in.src[0] = src;
        in.constant = constant;
        in.out = out;
        in.vec_mask = 0xf;
        return in;
    };
    VpInst last = vp_mov(5, vp_const(), 2);
    last.end = true;
    const std::vector<VpInst> prog = {vp_mov(0, vp_in()), vp_mov(1, vp_const(), 0), vp_mov(3, vp_const(), 1), last};
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM, assemble_vp(prog));
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
    b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, 1u << 0);
    b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
    std::vector<u32> words;
    for (const auto& v : c)
        for (float f : v) words.push_back(f2u(f));
    b.method(m::NV4097_SET_TRANSFORM_CONSTANT, words);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    state(b);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    return guest_pixel(e, kSurfaceOffset, 32, 40);
}

constexpr std::array<std::array<float, 4>, 3> kRedGreenFog = {{{1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f}, {0.5f, 0.f, 0.f, 0.f}}};

} // namespace

TEST_CASE("metal e2e: two-sided lighting colours back faces with the back colour, BFC0 (D-257)")
{
    REQUIRE_METAL();
    const std::array<u8, 4> red{0xff, 0xff, 0x00, 0x00}, green{0xff, 0x00, 0xff, 0x00};
    const auto draw = [](bool flip, bool two_sided, u32 front_face) {
        return draw_with_outputs(kRedGreenFog, 1, flip, [&](CommandBuilder& b) {
            b.method(m::NV4097_SET_TWO_SIDE_LIGHT_EN, two_sided ? 1u : 0u);
            b.method(m::NV4097_SET_FRONT_FACE, front_face);
        });
    };
    const u32 ccw = static_cast<u32>(FrontFace::CCW), cw = static_cast<u32>(FrontFace::CW);
    CHECK(draw(false, false, ccw) == red); // one-sided: COL0 on both faces
    CHECK(draw(true, false, ccw) == red);
    CHECK(draw(false, true, ccw) == red);  // two-sided, front face: COL0
    CHECK(draw(true, true, ccw) == green); // back face: BFC0
    CHECK(draw(false, true, cw) == green); // the same triangle is a back face when the front winds clockwise
}

TEST_CASE("metal e2e: FOGC carries the fog coordinate and the fog factor of NV4097_SET_FOG_MODE (D-256)")
{
    REQUIRE_METAL();
    // R0 = FOGC = (coordinate, factor, 0, 0): red is the coordinate, green the factor (RPCS3 RSXFragmentPrologue.glsl).
    const auto fog = [](u32 mode, float p0, float p1, float coord = 0.5f) {
        auto c = kRedGreenFog;
        c[2][0] = coord;
        return draw_with_outputs(c, 3, false, [&](CommandBuilder& b) {
            b.method(m::NV4097_SET_FOG_MODE, mode);
            b.method(m::NV4097_SET_FOG_PARAMS, {f2u(p0), f2u(p1)});
        });
    };
    const auto near = [](u8 got, double want) { return std::abs(got - want * 255.0) <= 2.0; };
    auto px = fog(0x2601, 1.25f, 1.f); // linear: 1 * 0.5 + (1.25 - 1)
    CHECK(near(px[1], 0.5));
    CHECK(near(px[2], 0.75));
    CHECK(px[3] == 0);
    CHECK(px[0] == 0);
    px = fog(0x0800, 1.5f, -0.1f); // exp: exp(11.084 * (-0.1 * 0.5 + 1.5 - 1.5))
    CHECK(near(px[2], std::exp(11.084 * -0.05)));
    px = fog(0x0801, 1.5f, 0.2f); // exp2: exp(-(4.709 * (0.2 * 0.5))^2)
    CHECK(near(px[2], std::exp(-std::pow(4.709 * 0.1, 2.0))));
    px = fog(0x0804, 1.25f, 1.f, -0.5f); // linear on |coordinate|; the coordinate itself is negative (red 0)
    CHECK(near(px[2], 0.75));
    CHECK(px[1] == 0);
    px = fog(0x2601, 1.25f, 1.f, -0.5f); // the same without abs: -0.25, clamped to 0
    CHECK(px[2] == 0);
    px = fog(0x2601, 2.f, 1.f); // 1.5, clamped to 1
    CHECK(px[2] == 0xff);
    px = fog(0x0802, 1.5f, -0.1f, -0.5f); // exp on |coordinate|
    CHECK(near(px[2], std::exp(11.084 * -0.05)));
}

TEST_CASE("metal e2e: window z is clipped to NV4097_SET_CLIP_MIN/MAX and stored as it is, or clamped there (D-258)")
{
    REQUIRE_METAL();
    // The triangle's window z is 0.75 (z 0.5, viewport scale and offset 0.5). Drawn into a Z16 depth buffer that
    // takes every depth: (whether colour 0 was drawn, the depth stored at the centre). With alpha_test the fragment
    // program can discard (D-941: Apple GPUs did not clamp such a fragment's depth).
    const auto draw = [](float clip_min, float clip_max, u32 zmin_max_control, bool alpha_test = false) {
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
        constexpr u32 kDepth = 0x180000;
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (1u << 5) | (1u << 8));
        b.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
        b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, kDepth);
        b.method(m::NV4097_SET_SURFACE_PITCH_Z, kW * 2);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xffff);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor | clear::kDepth);
        b.method(m::NV4097_SET_DEPTH_TEST_ENABLE, 1);
        b.method(m::NV4097_SET_DEPTH_FUNC, static_cast<u32>(CompareFunc::Always));
        b.method(m::NV4097_SET_DEPTH_MASK, 1);
        b.method(m::NV4097_SET_CLIP_MIN, {f2u(clip_min), f2u(clip_max)});
        b.method(m::NV4097_SET_ZMIN_MAX_CONTROL, zmin_max_control);
        if (alpha_test) {
            b.method(m::NV4097_SET_ALPHA_TEST_ENABLE, 1);
            b.method(m::NV4097_SET_ALPHA_FUNC, static_cast<u32>(CompareFunc::Always));
        }
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kDepth, kW * kH * 2));
        const bool drawn = guest_pixel(e, kSurfaceOffset, 32, 40)[1] == 0xff;
        const double depth = ps_read16(e.mem.base(), PS_MEM_RSX_LOCAL_BASE + kDepth + 40 * kW * 2 + 32 * 2) / 65535.0;
        return std::pair{drawn, depth};
    };
    const auto near = [](double got, double want) { return std::abs(got - want) < 2.0 / 65535.0; };
    auto [drawn, depth] = draw(0.f, 1.f, 1); // the reset state
    CHECK(drawn);
    CHECK(near(depth, 0.75));
    std::tie(drawn, depth) = draw(0.5f, 1.f, 1); // inside the range: stored as it is, not as its place in the range
    CHECK(drawn);
    CHECK(near(depth, 0.75));
    std::tie(drawn, depth) = draw(1.f, 0.5f, 1); // the ends either way round
    CHECK(drawn);
    CHECK(near(depth, 0.75));
    std::tie(drawn, depth) = draw(0.f, 0.7f, 1); // beyond the far end: clipped
    CHECK_FALSE(drawn);
    CHECK(depth == 1.0);
    std::tie(drawn, depth) = draw(0.8f, 1.f, 1); // before the near end: clipped
    CHECK_FALSE(drawn);
    std::tie(drawn, depth) = draw(0.f, 0.7f, 0x11); // clamped to the far end instead
    CHECK(drawn);
    CHECK(near(depth, 0.7));
    std::tie(drawn, depth) = draw(0.f, 0.7f, 0); // not clipped: clamped too (RPCS3)
    CHECK(drawn);
    CHECK(near(depth, 0.7));
    // A fragment program that can discard (an alpha test here) is clamped too, and clipped as before (D-941).
    std::tie(drawn, depth) = draw(0.f, 0.7f, 0x11, true);
    CHECK(drawn);
    CHECK(near(depth, 0.7));
    std::tie(drawn, depth) = draw(0.f, 0.7f, 1, true);
    CHECK_FALSE(drawn);
    std::tie(drawn, depth) = draw(0.f, 1.f, 1, true);
    CHECK(drawn);
    CHECK(near(depth, 0.75));
}

TEST_CASE("metal e2e: user clip planes keep what their distance has on the side they name (D-259)")
{
    REQUIRE_METAL();
    // o[5] = v[0]: plane 0's distance (o5.y) is the clip-space y, 1 at the top row, -1 at the bottom. Whether red was
    // drawn at (32, 20), above the middle, and at (32, 50), below it.
    const auto draw = [](u32 control, u32 output_mask) {
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
        const auto vp_mov = [](u32 out, VpSrc src, u32 constant = 0) {
            VpInst in;
            in.vec_op = 1;
            in.src[0] = src;
            in.constant = constant;
            in.out = out;
            in.vec_mask = 0xf;
            return in;
        };
        VpInst last = vp_mov(5, vp_in());
        last.end = true;
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM, assemble_vp({vp_mov(0, vp_in()), vp_mov(1, vp_const(), 0), last}));
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
        b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, 1u << 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT, {f2u(1.f), f2u(0.f), f2u(0.f), f2u(1.f)});
        b.method(m::NV4097_SET_USER_CLIP_PLANE_CONTROL, control);
        b.method(m::NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK, output_mask);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        return std::pair{guest_pixel(e, kSurfaceOffset, 32, 20)[1] == 0xff, guest_pixel(e, kSurfaceOffset, 32, 50)[1] == 0xff};
    };
    constexpr u32 kAll = 0x3fffff, kUc0 = 1u << 6;
    CHECK(draw(0, kAll) == std::pair{true, true});              // off
    CHECK(draw(2, kAll) == std::pair{true, false});             // plane 0, at or above zero: the top half
    CHECK(draw(1, kAll) == std::pair{false, true});             // plane 0, below zero: the bottom half
    CHECK(draw(2, kAll & ~kUc0) == std::pair{true, true});      // its distance is not put out: off
    CHECK(draw(2u << 4, kAll) == std::pair{true, true});        // plane 1 reads o5.z, the z of 0.5: kept everywhere
    CHECK(draw(1u << 4, kAll) == std::pair{false, false});      // ... or nowhere
    CHECK(draw(2 | (2u << 4), kAll) == std::pair{true, false}); // both: plane 0 decides
}

TEST_CASE("metal e2e: a constant or vertex base written between the ranges of one draw reaches only the ranges after it (D-260)")
{
    REQUIRE_METAL();
    // Two triangles in one BEGIN/END, the left one (vertices 0-2) and the right one (3-5); o[1] = c[0]. `between`
    // goes between their DRAW_ARRAYS. The pixels at the middle of the left and right triangles, and the draws made.
    struct Result {
        std::array<u8, 4> left, right;
        u64 draws, splits;
    };
    const auto draw = [](const std::function<void(CommandBuilder&)>& between, u32 second_first) {
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        const float xy[6][2] = {{-1.f, -1.f}, {0.f, -1.f}, {-0.5f, 1.f}, {0.f, -1.f}, {1.f, -1.f}, {0.5f, 1.f}};
        for (u32 v = 0; v < 6; v++) {
            e.put_local_f(kVertexOffset + v * 16 + 0, xy[v][0]);
            e.put_local_f(kVertexOffset + v * 16 + 4, xy[v][1]);
            e.put_local_f(kVertexOffset + v * 16 + 8, 0.5f);
            e.put_local_f(kVertexOffset + v * 16 + 12, 1.f);
        }
        const auto vp_mov = [](u32 out, VpSrc src, u32 constant = 0) {
            VpInst in;
            in.vec_op = 1;
            in.src[0] = src;
            in.constant = constant;
            in.out = out;
            in.vec_mask = 0xf;
            return in;
        };
        VpInst last = vp_mov(1, vp_const(), 0);
        last.end = true;
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM, assemble_vp({vp_mov(0, vp_in()), last}));
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
        b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, 1u << 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT, {f2u(1.f), f2u(0.f), f2u(0.f), f2u(1.f)}); // red
        set_fragment_program(b, kFpOffset, 0x40);
        b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0, static_cast<u32>(VertexType::F) | (4u << 4) | (16u << 8));
        b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0, kVertexOffset);
        b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::Triangles));
        b.method(m::NV4097_DRAW_ARRAYS, (2u << 24) | 0);
        between(b);
        b.method(m::NV4097_DRAW_ARRAYS, (2u << 24) | second_first);
        b.method(m::NV4097_SET_BEGIN_END, 0);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        return Result{guest_pixel(e, kSurfaceOffset, 16, 48), guest_pixel(e, kSurfaceOffset, 48, 48),
            e.backend->stats().draws, e.rsx->status().draw_splits};
    };
    const std::array<u8, 4> red{0xff, 0xff, 0x00, 0x00}, green{0xff, 0x00, 0xff, 0x00};
    // The colour changes between the triangles: the left one keeps red.
    Result r = draw([](CommandBuilder& b) {
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT, {f2u(0.f), f2u(1.f), f2u(0.f), f2u(1.f)});
    }, 3);
    CHECK(r.left == red);
    CHECK(r.right == green);
    CHECK(r.draws == 2);
    CHECK(r.splits == 1);
    // The same colour written again (and the load slot it already had): one draw.
    r = draw([](CommandBuilder& b) {
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT, {f2u(1.f), f2u(0.f), f2u(0.f), f2u(1.f)});
    }, 3);
    CHECK(r.left == red);
    CHECK(r.right == red);
    CHECK(r.draws == 1);
    CHECK(r.splits == 0);
    // The vertex base moves by three vertices: the second range's vertices 0-2 are the right triangle.
    r = draw([](CommandBuilder& b) { b.method(m::NV4097_SET_VERTEX_DATA_BASE_OFFSET, 3 * 16); }, 0);
    CHECK(r.left == red);
    CHECK(r.right == red);
    CHECK(r.draws == 2);
    // Without anything between them the two ranges are one draw.
    r = draw([](CommandBuilder&) {}, 3);
    CHECK(r.left == red);
    CHECK(r.right == red);
    CHECK(r.draws == 1);
}

TEST_CASE("metal e2e: the depth bounds test passes the pixels whose stored depth is inside the bounds (D-261)")
{
    REQUIRE_METAL();
    // A Z16 depth buffer cleared to 0.25; the triangle drawn red with depth 0.75 written, then drawn green under the
    // bounds test (depth test off). The centre (stored 0.75) and a corner outside the triangle (stored 0.25) after a
    // full-screen green draw under the same bounds: whether each is green.
    const auto draw = [](bool enable, float lo, float hi) {
        E2E e;
        const auto vp_mov = [](u32 out, VpSrc src, u32 constant = 0) {
            VpInst in;
            in.vec_op = 1;
            in.src[0] = src;
            in.constant = constant;
            in.out = out;
            in.vec_mask = 0xf;
            return in;
        };
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0, 0, 0});
        // Vertices 3-6: a full-screen quad at depth 0.5 (window 0.75 as well; its depth is not written).
        const float quad[4][2] = {{-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f}};
        for (u32 v = 0; v < 4; v++) {
            e.put_local_f(kVertexOffset + (3 + v) * 16 + 0, quad[v][0]);
            e.put_local_f(kVertexOffset + (3 + v) * 16 + 4, quad[v][1]);
            e.put_local_f(kVertexOffset + (3 + v) * 16 + 8, 0.5f);
            e.put_local_f(kVertexOffset + (3 + v) * 16 + 12, 1.f);
        }
        VpInst last = vp_mov(1, vp_const(), 0);
        last.end = true;
        constexpr u32 kDepth = 0x180000;
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (1u << 5) | (1u << 8));
        b.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
        b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, kDepth);
        b.method(m::NV4097_SET_SURFACE_PITCH_Z, kW * 2);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0x4000);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor | clear::kDepth);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM, assemble_vp({vp_mov(0, vp_in()), last}));
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
        b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, 1u << 0);
        b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0, static_cast<u32>(VertexType::F) | (4u << 4) | (16u << 8));
        b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0, kVertexOffset);
        set_fragment_program(b, kFpOffset, 0x40);
        // Pass 1: red, depth written
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT, {f2u(1.f), f2u(0.f), f2u(0.f), f2u(1.f)});
        b.method(m::NV4097_SET_DEPTH_TEST_ENABLE, 1);
        b.method(m::NV4097_SET_DEPTH_FUNC, static_cast<u32>(CompareFunc::Always));
        b.method(m::NV4097_SET_DEPTH_MASK, 1);
        draw_triangle(b);
        // Pass 2: green under the bounds, the depth left alone
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT, {f2u(0.f), f2u(1.f), f2u(0.f), f2u(1.f)});
        b.method(m::NV4097_SET_DEPTH_TEST_ENABLE, 0);
        b.method(m::NV4097_SET_DEPTH_MASK, 0);
        b.method(m::NV4097_SET_DEPTH_BOUNDS_TEST_ENABLE, enable ? 1u : 0u);
        b.method(m::NV4097_SET_DEPTH_BOUNDS_MIN, {f2u(lo), f2u(hi)});
        b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::Quads));
        b.method(m::NV4097_DRAW_ARRAYS, (3u << 24) | 3);
        b.method(m::NV4097_SET_BEGIN_END, 0);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        CHECK(e.backend->stats().depth_bounds_draws == (enable && (lo > 0.f || hi < 1.f) ? 1u : 0u));
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        const auto green = [&](u32 x, u32 y) { return guest_pixel(e, kSurfaceOffset, x, y)[2] == 0xff; };
        return std::pair{green(32, 40), green(2, 2)};
    };
    CHECK(draw(false, 0.5f, 1.f) == std::pair{true, true});  // off
    CHECK(draw(true, 0.5f, 1.f) == std::pair{true, false});  // 0.75 inside, 0.25 not
    CHECK(draw(true, 0.f, 0.5f) == std::pair{false, true});  // the other way round
    CHECK(draw(true, 0.2f, 0.8f) == std::pair{true, true});  // both inside
    CHECK(draw(true, 0.8f, 0.9f) == std::pair{false, false}); // neither
    CHECK(draw(true, 0.f, 1.f) == std::pair{true, true});    // the whole range: no copy needed
}

TEST_CASE("metal e2e: points take NV4097_SET_POINT_SIZE unless the program's size is on; sprites replace coordinates (D-262)")
{
    REQUIRE_METAL();
    // One point at the centre (32, 32): o[1] = c[0] (red); with `program_size` also o[6] = c[1] (x 16). `fp_tex0`:
    // R0 = TEX0 instead of COL0. Pixels (a, r, g, b) at `at` after the draw.
    const auto draw = [](bool program_size, u32 output_mask, u32 sprite_control, bool fp_tex0, std::vector<std::pair<u32, u32>> at) {
        E2E e;
        const auto vp_mov = [](u32 out, VpSrc src, u32 constant = 0) {
            VpInst in;
            in.vec_op = 1;
            in.src[0] = src;
            in.constant = constant;
            in.out = out;
            in.vec_mask = 0xf;
            return in;
        };
        FpInst mov;
        mov.op = 0x01;
        mov.dst = 0;
        mov.input = fp_tex0 ? 4 : 1;
        mov.src[0] = fp_in();
        mov.end = true;
        e.put_local_bytes(kFpOffset, assemble_fp({mov}));
        e.put_local_f(kVertexOffset + 0, 0.f);
        e.put_local_f(kVertexOffset + 4, 0.f);
        e.put_local_f(kVertexOffset + 8, 0.5f);
        e.put_local_f(kVertexOffset + 12, 1.f);
        std::vector<VpInst> prog = {vp_mov(0, vp_in()), vp_mov(1, vp_const(), 0)};
        if (program_size) prog.push_back(vp_mov(6, vp_const(), 1));
        prog.back().end = true;
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM, assemble_vp(prog));
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
        b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, 1u << 0);
        b.method(m::NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK, output_mask);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT,
            {f2u(1.f), f2u(0.f), f2u(0.f), f2u(1.f), f2u(16.f), f2u(0.f), f2u(0.f), f2u(0.f)});
        b.method(m::NV4097_SET_POINT_SIZE, f2u(8.f));
        b.method(m::NV4097_SET_POINT_SPRITE_CONTROL, sprite_control);
        set_fragment_program(b, kFpOffset, 0x40);
        b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0, static_cast<u32>(VertexType::F) | (4u << 4) | (16u << 8));
        b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0, kVertexOffset);
        b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::Points));
        b.method(m::NV4097_DRAW_ARRAYS, 0);
        b.method(m::NV4097_SET_BEGIN_END, 0);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        std::vector<std::array<u8, 4>> out;
        for (auto [x, y] : at) out.push_back(guest_pixel(e, kSurfaceOffset, x, y));
        return out;
    };
    const std::array<u8, 4> red{0xff, 0xff, 0x00, 0x00}, none{0, 0, 0, 0};
    constexpr u32 kAll = 0x3fffff, kPointSize = 1u << 5;
    // The register's 8: x and y 28 to 35.
    auto px = draw(false, kAll, 0, false, {{28, 28}, {35, 35}, {37, 37}, {27, 32}});
    CHECK(px == std::vector{red, red, none, none});
    // The program writes 16 and the output mask has its size: 24 to 39.
    px = draw(true, kAll, 0, false, {{24, 24}, {39, 39}, {40, 40}});
    CHECK(px == std::vector{red, red, none});
    // ... the mask without it: the register's 8 again.
    px = draw(true, kAll & ~kPointSize, 0, false, {{28, 28}, {37, 37}});
    CHECK(px == std::vector{red, none});
    // A sprite: TEX0 is the place in the point, (x, y, 1, 0): red and green rise to the right and down, blue 1, alpha 0.
    px = draw(false, kAll, 1u | (1u << 8), true, {{28, 28}, {35, 35}, {35, 28}});
    const auto near = [](u8 got, double want) { return std::abs(got - want * 255.0) <= 2.0; };
    CHECK(near(px[0][1], 0.5 / 8));
    CHECK(near(px[0][2], 0.5 / 8));
    CHECK(near(px[1][1], 7.5 / 8));
    CHECK(near(px[1][2], 7.5 / 8));
    CHECK(near(px[2][1], 7.5 / 8));
    CHECK(near(px[2][2], 0.5 / 8));
    CHECK(px[0][3] == 0xff);
    CHECK(px[0][0] == 0);
    // The sprite control off, or another coordinate in its mask: TEX0 is the vertex program's o[7], (0, 0, 0, 1) here.
    const std::array<u8, 4> unwritten{0xff, 0x00, 0x00, 0x00};
    px = draw(false, kAll, 1u << 8, true, {{35, 35}});
    CHECK(px[0] == unwritten);
    px = draw(false, kAll, 1u | (1u << 9), true, {{35, 35}});
    CHECK(px[0] == unwritten);
}

TEST_CASE("metal e2e: polygon stipple keeps the pixels whose pattern bit is set (D-263)")
{
    REQUIRE_METAL();
    // A checkerboard: even rows keep even x, odd rows odd x. Whether red was drawn at each pixel.
    const auto draw = [](bool stipple, std::vector<std::pair<u32, u32>> at) {
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        std::vector<u32> pattern(32);
        for (u32 y = 0; y < 32; y++) pattern[y] = y % 2 ? 0xaaaaaaaau : 0x55555555u;
        b.method(m::NV4097_SET_POLYGON_STIPPLE_PATTERN, pattern);
        b.method(m::NV4097_SET_POLYGON_STIPPLE, stipple ? 1u : 0u);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        std::vector<bool> out;
        for (auto [x, y] : at) out.push_back(guest_pixel(e, kSurfaceOffset, x, y)[1] == 0xff);
        return out;
    };
    const std::vector<std::pair<u32, u32>> at = {{32, 40}, {33, 40}, {32, 41}, {33, 41}};
    CHECK(draw(false, at) == std::vector{true, true, true, true});
    CHECK(draw(true, at) == std::vector{true, false, false, true});
}

TEST_CASE("metal e2e: the alpha test's reference is a half on a float16 target and a float on a float32 one (D-264)")
{
    REQUIRE_METAL();
    // The triangle's alpha is 0x99 / 255 (0.6); the test passes greater than the reference. Whether red was drawn.
    const auto draw = [](SurfaceColorFormat format, u32 bpp, u32 ref) {
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0xff000099, 0xff000099, 0xff000099});
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(format) | (2u << 5) | (1u << 8));
        b.method(m::NV4097_SET_SURFACE_PITCH_A, kW * bpp);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_ALPHA_TEST_ENABLE, 1);
        b.method(m::NV4097_SET_ALPHA_FUNC, static_cast<u32>(CompareFunc::Greater));
        b.method(m::NV4097_SET_ALPHA_REF, ref);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * bpp));
        const u8* p = e.mem.host(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset + (40 * kW + 32) * bpp);
        return bpp == 8 ? ps_ld16(p) != 0 : bpp == 16 ? ps_ld32(p) != 0 : p[1] != 0; // red
    };
    CHECK(draw(SurfaceColorFormat::F_W16Z16Y16X16, 8, 0x3800));      // 0.5: passes
    CHECK_FALSE(draw(SurfaceColorFormat::F_W16Z16Y16X16, 8, 0x3a00)); // 0.75: fails (its low byte, 0, would pass)
    CHECK(draw(SurfaceColorFormat::F_W32Z32Y32X32, 16, f2u(0.5f)));
    CHECK_FALSE(draw(SurfaceColorFormat::F_W32Z32Y32X32, 16, f2u(0.75f)));
    CHECK(draw(SurfaceColorFormat::A8R8G8B8, 4, 0x80));        // 128 / 255: passes
    CHECK_FALSE(draw(SurfaceColorFormat::A8R8G8B8, 4, 0xa0)); // 160 / 255: fails
}

TEST_CASE("metal e2e: NV4097_SET_WINDOW_OFFSET moves where the target starts in memory (D-265)")
{
    REQUIRE_METAL();
    E2E e;
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor); // the whole 64x64 at the surface's offset
    b.method(m::NV4097_SET_WINDOW_OFFSET, (4u << 16) | 8u); // x 8, y 4
    b.method(m::NV4097_SET_SURFACE_CLIP_HORIZONTAL, {16u << 16, 16u << 16});
    b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {16u << 16, 16u << 16});
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff00ff00); // green
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    const std::array<u8, 4> green{0xff, 0x00, 0xff, 0x00}, none{0, 0, 0, 0};
    CHECK(guest_pixel(e, kSurfaceOffset, 8, 4) == green);   // the window's first pixel
    CHECK(guest_pixel(e, kSurfaceOffset, 23, 19) == green); // its last
    CHECK(guest_pixel(e, kSurfaceOffset, 7, 4) == none);
    CHECK(guest_pixel(e, kSurfaceOffset, 8, 3) == none);
    CHECK(guest_pixel(e, kSurfaceOffset, 24, 19) == none);
    CHECK(guest_pixel(e, kSurfaceOffset, 23, 20) == none);
}

TEST_CASE("metal e2e: a fragment program's exported depth is clamped to [0, 1], in a float depth buffer too (D-266)")
{
    REQUIRE_METAL();
    // As the RSX does (RPCS3 VKFragmentProgram.cpp, from tests on the hardware); Metal clamps it to the viewport's range.
    // Z24S8 (a float depth buffer here). Pass 1 exports `first` in green; pass 2 exports `second` in red, tested with
    // `func` against what pass 1 stored. Whether the centre ends red.
    const auto draw = [](float first, float second, CompareFunc func) {
        E2E e;
        const auto program = [](std::array<float, 4> colour, float z) {
            FpInst c;
            c.op = 0x01;
            c.dst = 0;
            c.src[0] = fp_const();
            c.constant = colour;
            FpInst d = c;
            d.dst = 1;
            d.constant = std::array<float, 4>{0.f, 0.f, z, 0.f};
            d.end = true;
            return assemble_fp({c, d});
        };
        e.put_local_bytes(kFpOffset, program({0.f, 1.f, 0.f, 1.f}, first));
        e.put_local_bytes(kFp2Offset, program({1.f, 0.f, 0.f, 1.f}, second));
        write_triangle(e, {0, 0, 0});
        constexpr u32 kDepth = 0x180000;
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
        b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, kDepth);
        b.method(m::NV4097_SET_SURFACE_PITCH_Z, kW * 4);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xffffff00u);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor | clear::kDepth);
        upload_vertex_program(b);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_DEPTH_TEST_ENABLE, 1);
        b.method(m::NV4097_SET_DEPTH_MASK, 1);
        b.method(m::NV4097_SET_DEPTH_FUNC, static_cast<u32>(CompareFunc::Always));
        set_fragment_program(b, kFpOffset, 0x4e); // 32-bit exports and depth export
        draw_triangle(b);
        b.method(m::NV4097_SET_DEPTH_FUNC, static_cast<u32>(func));
        set_fragment_program(b, kFp2Offset, 0x4e);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        return guest_pixel(e, kSurfaceOffset, 32, 40)[1] == 0xff;
    };
    CHECK_FALSE(draw(1.5f, 1.f, CompareFunc::Less));   // 1.5 stored as 1: 1 is not less
    CHECK_FALSE(draw(-0.5f, 0.f, CompareFunc::Greater)); // -0.5 stored as 0: 0 is not greater
    CHECK(draw(0.75f, 0.5f, CompareFunc::Less));          // inside the range, as it is
}

TEST_CASE("metal e2e: an output the vertex output mask leaves out reaches the fragment program as (0, 0, 0, 1) (D-267)")
{
    REQUIRE_METAL();
    // o[1] = c[0] (red), o[7] = c[1] (green); the fragment program moves COL0 (input 1) or TEX0 (input 4) to R0.
    const auto draw = [](u32 output_mask, u32 fp_input) {
        E2E e;
        const auto vp_mov = [](u32 out, VpSrc src, u32 constant = 0) {
            VpInst in;
            in.vec_op = 1;
            in.src[0] = src;
            in.constant = constant;
            in.out = out;
            in.vec_mask = 0xf;
            return in;
        };
        FpInst mov;
        mov.op = 0x01;
        mov.dst = 0;
        mov.input = fp_input;
        mov.src[0] = fp_in();
        mov.end = true;
        e.put_local_bytes(kFpOffset, assemble_fp({mov}));
        write_triangle(e, {0, 0, 0});
        VpInst last = vp_mov(7, vp_const(), 1);
        last.end = true;
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM, assemble_vp({vp_mov(0, vp_in()), vp_mov(1, vp_const(), 0), last}));
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
        b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, 1u << 0);
        b.method(m::NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK, output_mask);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT,
            {f2u(1.f), f2u(0.f), f2u(0.f), f2u(1.f), f2u(0.f), f2u(1.f), f2u(0.f), f2u(1.f)});
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        return guest_pixel(e, kSurfaceOffset, 32, 40);
    };
    const std::array<u8, 4> red{0xff, 0xff, 0x00, 0x00}, green{0xff, 0x00, 0xff, 0x00}, unset{0xff, 0x00, 0x00, 0x00};
    constexpr u32 kAll = 0x3fffff, kFrontDiffuse = 1u << 0, kBackDiffuse = 1u << 2, kTex0 = 1u << 14;
    CHECK(draw(kAll, 1) == red);
    CHECK(draw(kAll & ~kFrontDiffuse, 1) == red);                  // either side's bit sends the colour
    CHECK(draw(kAll & ~(kFrontDiffuse | kBackDiffuse), 1) == unset); // neither: (0, 0, 0, 1)
    CHECK(draw(kAll, 4) == green);
    CHECK(draw(kAll & ~kTex0, 4) == unset);
    CHECK(draw(kTex0, 4) == green); // only what is read needs its bit
}

TEST_CASE("metal e2e: an indexed strip restarts at the restart index, which is no vertex (D-249)")
{
    REQUIRE_METAL();
    // Two quads as one triangle strip, 0 1 2 3 R 4 5 6 7: the left and the right quarter of the target. Vertex 8 is a
    // point near the top middle: read as a vertex, the restart index would draw triangles across the middle.
    const auto run = [](u32 restart_index, bool enable) {
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        const float xy[9][2] = {{-1.f, -1.f}, {-1.f, 1.f}, {-0.5f, -1.f}, {-0.5f, 1.f}, {0.5f, -1.f}, {0.5f, 1.f},
            {1.f, -1.f}, {1.f, 1.f}, {0.f, 0.9f}};
        for (u32 v = 0; v < 9; v++) {
            e.put_local_f(kVertexOffset + v * 16 + 0, xy[v][0]);
            e.put_local_f(kVertexOffset + v * 16 + 4, xy[v][1]);
            e.put_local_f(kVertexOffset + v * 16 + 8, 0.5f);
            e.put_local_f(kVertexOffset + v * 16 + 12, 1.f);
            e.put_local32(kColorOffset + v * 4, 0xff0000ff);
        }
        constexpr u32 kIndexOffset = 0x013000;
        const u16 indices[9] = {0, 1, 2, 3, static_cast<u16>(restart_index), 4, 5, 6, 7};
        std::vector<u8> be;
        for (u16 i : indices) be.insert(be.end(), {static_cast<u8>(i >> 8), static_cast<u8>(i)});
        e.put_local_bytes(kIndexOffset, be);
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_RESTART_INDEX_ENABLE, enable ? 1u : 0u);
        b.method(m::NV4097_SET_RESTART_INDEX, restart_index);
        b.method(m::NV4097_SET_INDEX_ARRAY_ADDRESS, {kIndexOffset, loc::kLocal | (1u << 4)}); // 16-bit indices
        b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::TriangleStrip));
        b.method(m::NV4097_DRAW_INDEX_ARRAY, (8u << 24) | 0);
        b.method(m::NV4097_SET_BEGIN_END, 0);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        const auto red = [&](u32 x, u32 y) { return guest_pixel(e, kSurfaceOffset, x, y)[1] == 0xff; };
        // The quads, and the middle near the top and the bottom rows (the stray triangles' side, whichever way up).
        return std::array<bool, 4>{red(8, 32) && red(56, 32), red(24, 13) || red(24, 51), red(40, 13) || red(40, 51),
            red(32, 32)};
    };
    for (const u32 restart : {8u, 0xffffu}) {
        INFO("restart index " << restart);
        const auto drawn = run(restart, true);
        CHECK(drawn[0]);       // both quads
        CHECK_FALSE(drawn[1]); // nothing between them
        CHECK_FALSE(drawn[2]);
        CHECK_FALSE(drawn[3]);
    }
    // Disabled: index 8 is a vertex, and the strip goes through it.
    const auto through = run(8, false);
    CHECK(through[0]);
    CHECK((through[1] || through[2]));
}

TEST_CASE("metal e2e: indexed draws run over the game's own indices, 16 and 32 bits, ranges and a base index (D-271)")
{
    REQUIRE_METAL();
    // Vertices 0-2 the left triangle, 3-5 the right one. Indices (big-endian): 0 1 2 X 3 4 5, drawn as two ranges, the
    // second starting at an odd index; then the right triangle again as 0 1 2 with a base index of 3.
    for (const bool wide : {false, true}) {
        INFO((wide ? "32-bit" : "16-bit"));
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        const float xy[6][2] = {{-1.f, -1.f}, {0.f, -1.f}, {-0.5f, 1.f}, {0.f, -1.f}, {1.f, -1.f}, {0.5f, 1.f}};
        for (u32 v = 0; v < 6; v++) {
            e.put_local_f(kVertexOffset + v * 16 + 0, xy[v][0]);
            e.put_local_f(kVertexOffset + v * 16 + 4, xy[v][1]);
            e.put_local_f(kVertexOffset + v * 16 + 8, 0.5f);
            e.put_local_f(kVertexOffset + v * 16 + 12, 1.f);
            e.put_local32(kColorOffset + v * 4, v < 3 ? 0xff0000ffu : 0x00ff00ffu); // left red, right green
        }
        constexpr u32 kIndexOffset = 0x013000;
        std::vector<u8> be;
        for (u32 i : {0u, 1u, 2u, 7u, 3u, 4u, 5u, 0u, 1u, 2u}) {
            if (wide) be.insert(be.end(), {static_cast<u8>(i >> 24), static_cast<u8>(i >> 16), static_cast<u8>(i >> 8), static_cast<u8>(i)});
            else be.insert(be.end(), {static_cast<u8>(i >> 8), static_cast<u8>(i)});
        }
        e.put_local_bytes(kIndexOffset, be);
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_INDEX_ARRAY_ADDRESS, {kIndexOffset, loc::kLocal | (wide ? 0u : 1u << 4)});
        b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::Triangles));
        b.method(m::NV4097_DRAW_INDEX_ARRAY, (2u << 24) | 0);
        b.method(m::NV4097_DRAW_INDEX_ARRAY, (2u << 24) | 4);
        b.method(m::NV4097_SET_BEGIN_END, 0);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        CHECK(e.backend->stats().ranges_indexed_by_metal == 2);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        CHECK(guest_pixel(e, kSurfaceOffset, 16, 48) == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00});
        CHECK(guest_pixel(e, kSurfaceOffset, 48, 48) == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00});
        // The right triangle alone, by indices 0 1 2 and a base index of 3, over a cleared target.
        auto c = e.at(b.offset());
        c.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        c.method(m::NV4097_SET_VERTEX_DATA_BASE_INDEX, 3);
        c.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::Triangles));
        c.method(m::NV4097_DRAW_INDEX_ARRAY, (2u << 24) | 7);
        c.method(m::NV4097_SET_BEGIN_END, 0);
        REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        CHECK(e.backend->stats().ranges_indexed_by_metal == 3);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        CHECK(guest_pixel(e, kSurfaceOffset, 16, 48) == std::array<u8, 4>{0, 0, 0, 0});
        CHECK(guest_pixel(e, kSurfaceOffset, 48, 48) == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00});
    }
}

TEST_CASE("metal e2e: a mesh of 400 vertices draws the same through 16-bit and 32-bit indices (D-271)")
{
    REQUIRE_METAL();
    // A 20x20 grid of vertices over the target, coloured by their index, as a triangle list (two per cell), the
    // vertices far apart in the index buffer as in a real mesh.
    const auto draw = [](bool wide) {
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        for (u32 v = 0; v < 400; v++) {
            const float x = -1.f + 2.f * static_cast<float>(v % 20) / 19.f, y = -1.f + 2.f * static_cast<float>(v / 20) / 19.f;
            e.put_local_f(kVertexOffset + v * 16 + 0, x);
            e.put_local_f(kVertexOffset + v * 16 + 4, y);
            e.put_local_f(kVertexOffset + v * 16 + 8, 0.5f);
            e.put_local_f(kVertexOffset + v * 16 + 12, 1.f);
            e.put_local32(kColorOffset + v * 4, ((v * 37) & 0xff) << 24 | ((v * 91) & 0xff) << 16 | (v & 0xff) << 8 | 0xff);
        }
        constexpr u32 kIndexOffset = 0x015000;
        std::vector<u8> be;
        u32 count = 0;
        for (u32 cy = 0; cy < 19; cy++)
            for (u32 cx = 0; cx < 19; cx++) {
                const u32 a = cy * 20 + cx;
                for (u32 i : {a, a + 1, a + 20, a + 1, a + 21, a + 20}) {
                    if (wide) be.insert(be.end(), {static_cast<u8>(i >> 24), static_cast<u8>(i >> 16), static_cast<u8>(i >> 8), static_cast<u8>(i)});
                    else be.insert(be.end(), {static_cast<u8>(i >> 8), static_cast<u8>(i)});
                    count++;
                }
            }
        e.put_local_bytes(kIndexOffset, be);
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 8, 0); // no texture coordinates here
        b.method(m::NV4097_SET_INDEX_ARRAY_ADDRESS, {kIndexOffset, loc::kLocal | (wide ? 0u : 1u << 4)});
        b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::Triangles));
        for (u32 first = 0; first < count; first += 256)
            b.method(m::NV4097_DRAW_INDEX_ARRAY, ((std::min(256u, count - first) - 1) << 24) | first);
        b.method(m::NV4097_SET_BEGIN_END, 0);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        CHECK(e.backend->stats().ranges_indexed_by_metal >= 1);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        std::vector<u8> pixels(kW * kH * 4);
        std::memcpy(pixels.data(), e.mem.host(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset), pixels.size());
        return pixels;
    };
    const auto narrow = draw(false), wide = draw(true);
    usize differ = 0;
    for (usize i = 0; i < narrow.size(); i++) differ += narrow[i] != wide[i];
    CHECK(differ == 0);
    usize lit = 0;
    for (usize i = 0; i < wide.size(); i += 4) lit += wide[i] != 0;
    CHECK(lit > kW * kH / 2); // the grid covers the target
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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

// Texture unit 0: a linear texture of the given format at a local memory offset, kW x kH unless given, identity remap.
void bind_linear_texture(CommandBuilder& b, u32 offset, u32 format, u32 pitch, u32 w = kW, u32 h = kH)
{
    b.method(m::NV4097_SET_TEXTURE_OFFSET, {
        offset,
        (loc::kLocal + 1) | (1u << 3) | (2u << 4) | ((format | tex::kLinearFlag) << 8) | (1u << 16),
        3u | (3u << 8) | (3u << 16),
        (1u << 31),
        tex::kRemapIdentity,
        (1u << 16) | (1u << 24),
        (w << 16) | h,
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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

    // The CPU writes another program at the same address: the next batch decodes and compiles it, and the vertex
    // program again to put out what it reads (TEX0, not COL0, D-269).
    e.put_local_bytes(kFpOffset, fp_texture());
    auto c = e.at(b.offset());
    draw_triangle(c);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().programs_compiled == compiled + 2);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff}); // the texture's blue
}

TEST_CASE("metal e2e: a fragment program written again where another was is the one decoded before (D-240)")
{
    // GTA V writes its programs into memory it reuses: a program found again at an address is not decoded again, and
    // keeps its compile (run 221: decoding them again was a fifth of the RSX thread's work).
    REQUIRE_METAL();
    E2E e;
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red
    for (u32 i = 0; i < kW * kH; i++) e.put_local32(kTexOffset + i * 4, 0xff0000ffu); // blue texels
    const auto draw = [&](const std::vector<u8>& program, u32 at) {
        e.put_local_bytes(kFpOffset, program);
        auto b = e.at(at);
        if (at == 0) {
            setup_surface(b, kSurfaceOffset);
            upload_vertex_program(b);
            bind_vertex_arrays(b);
            bind_linear_texture(b, kTexOffset, tex::A8R8G8B8, kW * 4);
        }
        set_fragment_program(b, kFpOffset, 0x40);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        CHECK(e.failures.empty());
        return b.offset();
    };
    u32 at = draw(fp_color(false), 0);
    at = draw(fp_texture(), at);
    const u64 decoded = e.backend->stats().fragment_programs_decoded;
    const u64 compiled = e.backend->stats().programs_compiled;
    CHECK(decoded == 2);
    // The first program back at the same address: found, not decoded or compiled again, and it draws.
    at = draw(fp_color(false), at);
    CHECK(e.backend->stats().fragment_programs_decoded == decoded);
    CHECK(e.backend->stats().fragment_programs_found_again == 1);
    CHECK(e.backend->stats().programs_compiled == compiled);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00}); // red
    // And the textured one again: blue.
    draw(fp_texture(), at);
    CHECK(e.backend->stats().fragment_programs_decoded == decoded);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff});
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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

namespace {

// One triangle with a vertex program whose colour comes from c[0] (red) or c[1] (green) by a BRB or CLB on bit 2 of
// NV4097_SET_TRANSFORM_BRANCH_BITS; the pixel inside it, read back (A R G B bytes).
std::array<u8, 4> draw_with_branch_bits(bool call, bool if_true, u32 bits)
{
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    const auto mov = [](u32 out, VpSrc src, u32 constant = 0) {
        VpInst in;
        in.vec_op = 1;
        in.src[0] = src;
        in.constant = constant;
        in.out = out;
        in.vec_mask = 0xf;
        return in;
    };
    std::vector<VpInst> prog;
    if (!call) {
        VpInst brb; // 0: BRB L3 on b2
        brb.sca_op = 0x11;
        brb.branch_target = 3;
        brb.branch_index = 2;
        brb.branch_if_true = if_true;
        prog.push_back(brb);
        prog.push_back(mov(1, vp_const(), 0)); // 1: MOV o[1], c[0]
        VpInst bri; // 2: BRI L4
        bri.sca_op = 0x09;
        bri.branch_target = 4;
        prog.push_back(bri);
        prog.push_back(mov(1, vp_const(), 1)); // 3: MOV o[1], c[1]
        VpInst pos = mov(0, vp_in());          // 4: MOV o[0], v[0] END
        pos.end = true;
        prog.push_back(pos);
    } else {
        prog.push_back(mov(1, vp_const(), 0)); // 0: MOV o[1], c[0]
        VpInst clb; // 1: CLB L3 on b2
        clb.sca_op = 0x12;
        clb.branch_target = 3;
        clb.branch_index = 2;
        clb.branch_if_true = if_true;
        prog.push_back(clb);
        VpInst pos = mov(0, vp_in()); // 2: MOV o[0], v[0] END
        pos.end = true;
        prog.push_back(pos);
        VpInst sub = mov(1, vp_const(), 1); // 3: MOV o[1], c[1] ; RET END (the last instruction holds the end bit)
        sub.sca_op = 0x0c;
        sub.end = true;
        prog.push_back(sub);
    }
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM, assemble_vp(prog));
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
    b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, 1u << 0);
    b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
    b.method(m::NV4097_SET_TRANSFORM_CONSTANT,
        {f2u(1.f), f2u(0.f), f2u(0.f), f2u(1.f), f2u(0.f), f2u(1.f), f2u(0.f), f2u(1.f)});
    b.method(m::NV4097_SET_TRANSFORM_BRANCH_BITS, bits);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().draws == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    return guest_pixel(e, kSurfaceOffset, 32, 48);
}

} // namespace

TEST_CASE("metal e2e: vertex programs branch and call on NV4097_SET_TRANSFORM_BRANCH_BITS (BRB, CLB, D-113)")
{
    REQUIRE_METAL();
    const std::array<u8, 4> red{0xff, 0xff, 0x00, 0x00}, green{0xff, 0x00, 0xff, 0x00};
    CHECK(draw_with_branch_bits(false, true, 1u << 2) == green); // BRB taken: bit 2 set
    CHECK(draw_with_branch_bits(false, true, 1u << 1) == red);   // not taken: bit 2 clear
    CHECK(draw_with_branch_bits(false, false, 0) == green);      // BRB on a clear bit, taken
    CHECK(draw_with_branch_bits(true, true, 1u << 2) == green);  // CLB calls the subroutine, which returns
    CHECK(draw_with_branch_bits(true, true, 0) == red);
    CHECK(draw_with_branch_bits(true, false, 0) == green);
}

TEST_CASE("metal e2e: a transform constant written between two draws of one batch reaches the second (D-129)")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    VpInst colour; // MOV o[1], c[0]
    colour.vec_op = 1;
    colour.src[0] = vp_const();
    colour.out = 1;
    colour.vec_mask = 0xf;
    VpInst pos; // MOV o[0], v[0] END
    pos.vec_op = 1;
    pos.src[0] = vp_in();
    pos.out = 0;
    pos.vec_mask = 0xf;
    pos.end = true;
    auto b = e.at(0);
    const auto draw_in = [&](u32 offset, std::array<float, 4> c0) {
        setup_surface(b, offset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM, assemble_vp({colour, pos}));
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
        b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, 1u << 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT, {f2u(c0[0]), f2u(c0[1]), f2u(c0[2]), f2u(c0[3])});
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
    };
    // One batch: red into the first surface, then green into the second, then (constants untouched) the third.
    draw_in(kSurfaceOffset, {1.f, 0.f, 0.f, 1.f});
    draw_in(kSurfaceOffset + 0x100000, {0.f, 1.f, 0.f, 1.f});
    setup_surface(b, kSurfaceOffset + 0x200000);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().draws == 3);
    for (u32 i = 0; i < 3; i++) REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset + i * 0x100000, kW * kH * 4));
    const std::array<u8, 4> red{0xff, 0xff, 0x00, 0x00}, green{0xff, 0x00, 0xff, 0x00};
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == red);
    CHECK(guest_pixel(e, kSurfaceOffset + 0x100000, 32, 48) == green);
    CHECK(guest_pixel(e, kSurfaceOffset + 0x200000, 32, 48) == green);
}

TEST_CASE("metal e2e: vertex programs uploaded again before each draw are found by their words (D-210)")
{
    // Assassin's Creed uploads its vertex programs again before nearly every draw (2,150 draws a frame in a crowd);
    // the backend finds the one uploaded among those it decoded lately. Two programs at the same start alternate, then
    // one that differs from the first in one word only (the constant it reads).
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    const auto program = [](u32 constant) {
        VpInst colour; // MOV o[1], c[constant]
        colour.vec_op = 1;
        colour.src[0] = vp_const();
        colour.constant = constant;
        colour.out = 1;
        colour.vec_mask = 0xf;
        VpInst pos; // MOV o[0], v[0] END
        pos.vec_op = 1;
        pos.src[0] = vp_in();
        pos.out = 0;
        pos.vec_mask = 0xf;
        pos.end = true;
        return assemble_vp({colour, pos});
    };
    auto b = e.at(0);
    b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
    b.method(m::NV4097_SET_TRANSFORM_CONSTANT, {f2u(1.f), 0, 0, f2u(1.f), 0, f2u(1.f), 0, f2u(1.f), 0, 0, f2u(1.f), f2u(1.f)});
    const u32 constants[] = {0, 1, 0, 1, 2};
    for (u32 i = 0; i < 5; i++) {
        setup_surface(b, kSurfaceOffset + i * 0x100000);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM, program(constants[i]));
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
        b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, 1u << 0);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
    }
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    const std::array<u8, 4> red{0xff, 0xff, 0x00, 0x00}, green{0xff, 0x00, 0xff, 0x00}, blue{0xff, 0x00, 0x00, 0xff};
    const std::array<u8, 4> expected[] = {red, green, red, green, blue};
    for (u32 i = 0; i < 5; i++) {
        INFO("draw " << i);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset + i * 0x100000, kW * kH * 4));
        CHECK(guest_pixel(e, kSurfaceOffset + i * 0x100000, 32, 48) == expected[i]);
    }
}

TEST_CASE("metal e2e: only the transform constants a vertex program reads are uploaded (D-278)")
{
    // A program reading c[40], one reading c[5], the first again after c[40] changed: each upload copies the one
    // constant its program reads, at its own place, and each draw gets its colour.
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    const auto program = [](u32 constant) {
        VpInst colour; // MOV o[1], c[constant]
        colour.vec_op = 1;
        colour.src[0] = vp_const();
        colour.constant = constant;
        colour.out = 1;
        colour.vec_mask = 0xf;
        VpInst pos; // MOV o[0], v[0] END
        pos.vec_op = 1;
        pos.src[0] = vp_in();
        pos.out = 0;
        pos.vec_mask = 0xf;
        pos.end = true;
        return assemble_vp({colour, pos});
    };
    auto b = e.at(0);
    const auto constant = [&](u32 slot, float r, float g, float bl) {
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, slot);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT, {f2u(r), f2u(g), f2u(bl), f2u(1.f)});
    };
    constant(40, 1.f, 0.f, 0.f);
    constant(5, 0.f, 1.f, 0.f);
    const u32 reads[] = {40, 5, 40};
    for (u32 i = 0; i < 3; i++) {
        if (i == 2) constant(40, 0.f, 0.f, 1.f);
        setup_surface(b, kSurfaceOffset + i * 0x100000);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM, program(reads[i]));
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
        b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, 1u << 0);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
    }
    const u64 before = e.backend->stats().vertex_constant_bytes_copied;
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().vertex_constant_bytes_copied - before == 3 * 16);
    const std::array<u8, 4> expected[] = {{0xff, 0xff, 0x00, 0x00}, {0xff, 0x00, 0xff, 0x00}, {0xff, 0x00, 0x00, 0xff}};
    for (u32 i = 0; i < 3; i++) {
        INFO("draw " << i);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset + i * 0x100000, kW * kH * 4));
        CHECK(guest_pixel(e, kSurfaceOffset + i * 0x100000, 32, 48) == expected[i]);
    }
}

TEST_CASE("metal e2e: interleaved vertex attributes are copied once and read right (D-211)")
{
    // Position (four floats) and colour (four bytes) in one 20-byte vertex: the two attributes' ranges overlap, and
    // the vertex data is copied once (60 bytes, not 56 + 44).
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    const float xy[3][2] = {{-1.f, -1.f}, {1.f, -1.f}, {0.f, 1.f}};
    for (u32 v = 0; v < 3; v++) {
        e.put_local_f(kVertexOffset + v * 20 + 0, xy[v][0]);
        e.put_local_f(kVertexOffset + v * 20 + 4, xy[v][1]);
        e.put_local_f(kVertexOffset + v * 20 + 8, 0.5f);
        e.put_local_f(kVertexOffset + v * 20 + 12, 1.f);
        e.put_local32(kVertexOffset + v * 20 + 16, 0x00ff00ffu); // RGBA: green
    }
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0, static_cast<u32>(VertexType::F) | (4u << 4) | (20u << 8));
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0, kVertexOffset);
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3, static_cast<u32>(VertexType::UB) | (4u << 4) | (20u << 8));
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3, kVertexOffset + 16);
    const u64 before = e.backend->stats().vertex_bytes_copied;
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().vertex_bytes_copied - before == 60);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == (std::array<u8, 4>{0xff, 0x00, 0xff, 0x00}));
}

TEST_CASE("metal e2e: PSPOCKET_RSX_CHECK_SURFACES reports a surface sampled after its memory changed under it")
{
    REQUIRE_METAL();
    setenv("PSPOCKET_RSX_CHECK_SURFACES", "1", 1);
    E2E e;
    unsetenv("PSPOCKET_RSX_CHECK_SURFACES");
    e.put_local_bytes(kFpOffset, fp_color(false));
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0x00ff00ff, 0x00ff00ff, 0x00ff00ff});
    auto b = e.at(0);
    setup_surface(b, kSurfaceB);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    // A write the backend is not told of (as a CPU store or an SPU DMA), then a texture over the surface.
    e.put_local32(kSurfaceB + 100, 0x12345678u);
    auto c = e.at(b.offset());
    setup_surface(c, kSurfaceOffset);
    c.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    set_fragment_program(c, kFp2Offset, 0x40);
    bind_linear_texture(c, kSurfaceB, tex::A8R8G8B8, kW * 4);
    draw_triangle(c);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    CHECK(e.backend->stats().surface_samples == 1);
    CHECK(e.backend->stats().surface_guest_changes == 1);
}

TEST_CASE("metal e2e: the shader cache records what draws compile and makes it ahead for the next backend (D-114)")
{
    // The first mission compiled 533 programs and pipelines in its first minutes, 8.3 s of stalls on the Mac (run 082).
    REQUIRE_METAL();
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / std::format("pspocket-shader-cache-{}", static_cast<long>(getpid()));
    std::filesystem::remove_all(dir);
    const auto draw = [](E2E& e) {
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00});
    };
    {
        E2E e(0, dir.string());
        e.backend->wait_for_shader_cache(); // nothing recorded yet
        draw(e);
        CHECK(e.backend->stats().programs_compiled == 2);
        CHECK(e.backend->stats().pipelines_created == 1);
        CHECK(e.backend->stats().programs_prewarmed == 0);
    }
    CHECK(std::distance(std::filesystem::directory_iterator(dir / "programs"), std::filesystem::directory_iterator{}) == 2);
    {
        E2E e(0, dir.string());
        e.backend->wait_for_shader_cache();
        draw(e);
        CHECK(e.backend->stats().programs_compiled == 0); // made ahead, not at the draw
        CHECK(e.backend->stats().pipelines_created == 0);
        CHECK(e.backend->stats().programs_prewarmed == 2);
        CHECK(e.backend->stats().pipelines_prewarmed == 1);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("metal e2e: with background compiles, a draw goes without its programs until they are made (D-158)")
{
    // A game the phone has never seen: nothing in the shader cache, and no frame may wait for the Metal compiler.
    REQUIRE_METAL();
    const auto draw = [](E2E& e, u32 offset) {
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
        auto b = e.at(offset);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        return guest_pixel(e, kSurfaceOffset, 32, 48);
    };
    const std::array<u8, 4> red{0xff, 0xff, 0x00, 0x00}, cleared{0, 0, 0, 0};
    {
        E2E e(0, {}, 1800, true, 0.0, false); // no wait at all, no shader interpreter (D-940)
        CHECK(draw(e, 0) == cleared);  // skipped: its vertex program is compiling
        CHECK(e.backend->stats().draws_compiling == 1);
        CHECK(e.backend->stats().draws == 0);
        // Every program and pipeline it needs, one at a time: each later draw finds one more.
        for (u32 i = 1; i <= 3 && e.backend->stats().draws == 0; i++) {
            e.backend->wait_for_async_compiles();
            draw(e, 0x1000 * i);
        }
        CHECK(e.backend->stats().draws == 1);
        CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == red);
        CHECK(e.backend->stats().async_programs == 2);
        CHECK(e.backend->stats().async_pipelines == 1);
        CHECK(e.backend->stats().compile_ns == 0); // nothing compiled on the RSX thread
        CHECK(e.backend->stats().pipeline_ns == 0);
    }
    {
        E2E e(0, {}, 1800, true, 10000.0, false); // a budget that covers the compiles: the draw waits and draws
        CHECK(draw(e, 0) == red);
        CHECK(e.backend->stats().draws_compiling == 0);
        CHECK(e.backend->stats().draws == 1);
        CHECK(e.backend->stats().async_programs == 2);
        CHECK(e.backend->stats().compile_wait_ns > 0);
    }
}

TEST_CASE("metal e2e: with background compiles, a draw whose programs or pipeline are compiling is drawn by the shader "
          "interpreter (D-940)")
{
    REQUIRE_METAL();
    const auto draw = [](E2E& e, u32 offset, bool fp16) {
        e.put_local_bytes(kFpOffset, fp_color(fp16));
        write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
        auto b = e.at(offset);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, fp16 ? 0 : 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        return guest_pixel(e, kSurfaceOffset, 32, 48);
    };
    const std::array<u8, 4> red{0xff, 0xff, 0x00, 0x00}, cleared{0, 0, 0, 0};
    E2E e(0, {}, 1800, true, 0.0); // no wait at all
    e.backend->wait_for_async_compiles(); // the interpreter's functions, queued when the backend starts
    // Its programs compiling, and the interpreter's pipeline for these targets: left out.
    CHECK(draw(e, 0, false) == cleared);
    CHECK(e.backend->stats().draws_compiling == 1);
    // Its own pipeline compiling: the interpreter draws it.
    e.backend->wait_for_async_compiles();
    CHECK(draw(e, 0x1000, false) == red);
    CHECK(e.backend->stats().draws_interpreted == 1);
    // Everything made: its own programs draw it.
    e.backend->wait_for_async_compiles();
    CHECK(draw(e, 0x2000, false) == red);
    CHECK(e.backend->stats().draws == 2);
    CHECK(e.backend->stats().draws_interpreted == 1);
    // A program never seen: drawn at once, by the interpreter, while it compiles.
    CHECK(draw(e, 0x3000, true) == red);
    CHECK(e.backend->stats().draws == 3);
    CHECK(e.backend->stats().draws_interpreted == 2);
    CHECK(e.backend->stats().draws_compiling == 1);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().surface_samples == 1);
    CHECK(e.backend->stats().textures_decoded == 0);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff});
}

TEST_CASE("metal e2e: a clear writes its value into an A8B8G8R8 target's words as it is, channel mask too (D-119)")
{
    // GTA V clears its characters' damage target, A8B8G8R8, to 0x00ff0000 and reads it as A8R8G8B8: transparent red.
    // Taken as red, green, blue, the clear made it transparent blue to that texture, and the characters red.
    REQUIRE_METAL();
    E2E e;
    auto b = e.at(0);
    setup_surface_as(b, kSurfaceB, SurfaceColorFormat::A8B8G8R8, kW * 4);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0x11223344);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceB, 5, 7) == std::array<u8, 4>{0x11, 0x22, 0x33, 0x44});

    // The mask's red is the value's red byte (the second): the target's blue.
    auto c = e.at(b.offset());
    c.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    c.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    c.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0x00ff0000);
    c.method(m::NV4097_CLEAR_SURFACE, clear::kRed);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceB, 5, 7) == std::array<u8, 4>{0x00, 0xff, 0x00, 0x00});
}

// An A8R8G8B8 target at a local offset with its own clip and pitch; the scissor covers it (D-120).
void setup_surface_sized(CommandBuilder& b, u32 offset, u32 width, u32 height, u32 pitch)
{
    setup_surface(b, offset);
    b.method(m::NV4097_SET_SURFACE_PITCH_A, pitch);
    b.method(m::NV4097_SET_SURFACE_CLIP_HORIZONTAL, {width << 16, height << 16});
    b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {width << 16, height << 16});
}

// Guest bytes (a, r, g, b) of pixel (x, y) of an A8R8G8B8 target with that pitch, after a flush.
std::array<u8, 4> guest_pixel_pitch(E2E& e, u32 surface_offset, u32 pitch, u32 x, u32 y)
{
    const u8* p = e.mem.host(PS_MEM_RSX_LOCAL_BASE + surface_offset + y * pitch + x * 4);
    return {p[0], p[1], p[2], p[3]};
}

TEST_CASE("metal e2e: a multisampled target is drawn one texel per sample, its samples side by side in memory (D-250)")
{
    REQUIRE_METAL();
    // A kW x kH target in AA mode 3 (two samples across) or 4 (two by two): memory holds 2 kW x kH or 2 kW x 2 kH
    // texels with the pitch of the wider row. The usual triangle, red, the scissor the whole target or its left half.
    const auto run = [](u32 mode, bool left_half) {
        const u32 sx = 2, sy = mode == 4 ? 2 : 1, pitch = kW * sx * 4;
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
        auto b = e.at(0);
        setup_surface_sized(b, kSurfaceOffset, kW, kH, pitch);
        b.method(m::NV4097_SET_SURFACE_FORMAT,
            static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (2u << 5) | (1u << 8) | (mode << 12));
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        if (left_half) b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {(kW / 2) << 16, kH << 16});
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, pitch * kH * sy));
        std::vector<std::vector<bool>> red(kH * sy, std::vector<bool>(kW * sx));
        for (u32 y = 0; y < kH * sy; y++)
            for (u32 x = 0; x < kW * sx; x++) red[y][x] = guest_pixel_pitch(e, kSurfaceOffset, pitch, x, y)[1] == 0xff;
        return red;
    };
    for (const u32 mode : {3u, 4u}) {
        INFO("AA mode " << mode);
        const u32 sx = 2, sy = mode == 4 ? 2 : 1;
        const auto red = run(mode, false);
        u32 total = 0, mixed = 0;
        for (const auto& row : red)
            for (bool r : row) total += r;
        for (u32 y = 0; y < kH; y++) {
            for (u32 x = 0; x < kW; x++) {
                u32 n = 0;
                for (u32 j = 0; j < sy; j++)
                    for (u32 i = 0; i < sx; i++) n += red[y * sy + j][x * sx + i];
                mixed += n != 0 && n != sx * sy;
            }
        }
        const u32 texels = kW * sx * kH * sy;
        CHECK(total > texels * 4 / 10); // about half: the triangle fills the target's lower half's width
        CHECK(total < texels * 6 / 10);
        CHECK(mixed > 0); // an edge falls between a pixel's samples: each sample is covered on its own
        CHECK(red[kH * sy * 3 / 4][kW * sx / 2]);      // inside, near the base's middle
        CHECK_FALSE(red[kH * sy / 8][kW * sx / 16]);   // the top-left corner stays clear
        // The scissor is in pixels: its left half is the samples' left half.
        const auto half = run(mode, true);
        bool right = false, left = false;
        for (u32 y = 0; y < kH * sy; y++) {
            for (u32 x = 0; x < kW * sx; x++) (x < kW * sx / 2 ? left : right) |= half[y][x];
        }
        CHECK(left);
        CHECK_FALSE(right);
    }
}

TEST_CASE("metal e2e: a point on a two-by-two multisampled target covers its size in pixels, twice that in samples (D-275)")
{
    REQUIRE_METAL();
    // One point of size 4 at the centre of a kW x kH target in AA mode 4: 8x8 samples around sample (64, 64).
    const u32 sx = 2, sy = 2, pitch = kW * sx * 4;
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    e.put_local_f(kVertexOffset + 0, 0.f);
    e.put_local_f(kVertexOffset + 4, 0.f);
    auto b = e.at(0);
    setup_surface_sized(b, kSurfaceOffset, kW, kH, pitch);
    b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (2u << 5) | (1u << 8) | (4u << 12));
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    b.method(m::NV4097_SET_POINT_SIZE, f2u(4.f));
    b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::Points));
    b.method(m::NV4097_DRAW_ARRAYS, 0);
    b.method(m::NV4097_SET_BEGIN_END, 0);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, pitch * kH * sy));
    const auto red = [&](u32 x, u32 y) { return guest_pixel_pitch(e, kSurfaceOffset, pitch, x, y)[1] == 0xff; };
    CHECK(red(60, 60));   // the point's first sample row and column (4 pixels: 8 samples, 60 to 67)
    CHECK(red(67, 67));
    CHECK_FALSE(red(59, 64));
    CHECK_FALSE(red(64, 68));
}

TEST_CASE("metal e2e: WPOS on a multisampled target is the pixel's position, the same for all its samples (D-276)")
{
    REQUIRE_METAL();
    // R0 = WPOS * (1/64, 0, 0, 0) + (1/64, 0, 0, 0) (one embedded constant serves both): red is (x + 1) / 64 for the
    // pixel's x. AA mode 4: two by two samples a pixel.
    const u32 sx = 2, sy = 2, pitch = kW * sx * 4;
    E2E e;
    FpInst mad;
    mad.op = 0x04; // MAD
    mad.dst = 0;
    mad.input = 0; // WPOS
    mad.src[0] = fp_in();
    mad.src[1] = fp_const();
    mad.constant = std::array<float, 4>{1.f / 64.f, 0.f, 0.f, 0.f};
    mad.src[2] = fp_const();
    e.put_local_bytes(kFpOffset, assemble_fp({[&] { mad.end = true; return mad; }()}));
    write_triangle(e, {0, 0, 0});
    auto b = e.at(0);
    setup_surface_sized(b, kSurfaceOffset, kW, kH, pitch);
    b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (2u << 5) | (1u << 8) | (4u << 12));
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, pitch * kH * sy));
    // Pixel (24, 48) and (40, 48), inside the triangle: each of their four samples holds (x + 0.5 + 1) / 64.
    for (const u32 px : {24u, 40u}) {
        const double want = (px + 0.5 + 1.0) / 64.0 * 255.0;
        for (u32 j = 0; j < sy; j++)
            for (u32 i = 0; i < sx; i++) {
                const auto p = guest_pixel_pitch(e, kSurfaceOffset, pitch, px * sx + i, 48 * sy + j);
                INFO("pixel " << px << " sample " << i << "," << j << " red " << int(p[1]));
                CHECK(std::abs(p[1] - want) <= 1.0);
            }
    }
}

TEST_CASE("metal e2e: a target narrower than the one at its address is its left part, and the rest stays (D-120)")
{
    // GTA V clears the right half of a 2560x512 target for its characters, then draws into the left half as a
    // 1280x512 target with the same pitch. A new surface for the narrow one lost the cleared half: they turned black.
    REQUIRE_METAL();
    E2E e;
    constexpr u32 kPitch = 2 * kW * 4;
    auto b = e.at(0);
    setup_surface_sized(b, kSurfaceB, 2 * kW, kH, kPitch);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff112233);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {(kW << 16) | kW, kH << 16}); // the right half
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff445566);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    setup_surface_sized(b, kSurfaceB, kW, kH, kPitch);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff778899);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kPitch * kH));
    CHECK(guest_pixel_pitch(e, kSurfaceB, kPitch, 5, 7) == std::array<u8, 4>{0xff, 0x77, 0x88, 0x99});
    CHECK(guest_pixel_pitch(e, kSurfaceB, kPitch, kW + 5, 7) == std::array<u8, 4>{0xff, 0x44, 0x55, 0x66});
    CHECK(e.backend->stats().surfaces_created == 1);
}

TEST_CASE("metal e2e: a target wider than the one at its address keeps what that one held (D-120)")
{
    REQUIRE_METAL();
    E2E e;
    constexpr u32 kPitch = 2 * kW * 4;
    auto b = e.at(0);
    setup_surface_sized(b, kSurfaceB, kW, kH, kPitch);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff778899);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    setup_surface_sized(b, kSurfaceB, 2 * kW, kH, kPitch);
    b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {(kW << 16) | kW, kH << 16});
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff445566);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().surfaces_grown == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kPitch * kH));
    CHECK(guest_pixel_pitch(e, kSurfaceB, kPitch, 5, 7) == std::array<u8, 4>{0xff, 0x77, 0x88, 0x99});
    CHECK(guest_pixel_pitch(e, kSurfaceB, kPitch, kW + 5, 7) == std::array<u8, 4>{0xff, 0x44, 0x55, 0x66});
}

TEST_CASE("metal e2e: a texture smaller than the target at its address samples the target's top-left corner (D-120)")
{
    REQUIRE_METAL();
    E2E e;
    constexpr u32 kPitch = 2 * kW * 4;
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    auto b = e.at(0);
    // A target twice as wide: green on the left, red on the right
    setup_surface_sized(b, kSurfaceB, 2 * kW, kH, kPitch);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff00ff00);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {(kW << 16) | kW, kH << 16});
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xffff0000);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    // Its left half read as a kW x kH texture with its pitch: green wherever the triangle samples it
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFp2Offset, 0x40);
    bind_vertex_arrays(b);
    bind_linear_texture(b, kSurfaceB, tex::A8R8G8B8, kPitch);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().surface_corners == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 5, 60) == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00});  // u near 0
    CHECK(guest_pixel(e, kSurfaceOffset, 58, 60) == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00}); // u near 1
}

TEST_CASE("metal e2e: a target inside a larger one keeps it, and each sees what the other wrote (D-122)")
{
    // GTA V clears a 640x256 target 256 rows into its 2560x512 damage target, same pitch, and the first mission's
    // guard samples the large one: a new surface for the small one evicted the large one, and he came out black.
    REQUIRE_METAL();
    E2E e;
    constexpr u32 kPitch = 2 * kW * 4;
    constexpr u32 kChild = kSurfaceB + (kH / 2) * kPitch; // half way down, at the left
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    auto b = e.at(0);
    setup_surface_sized(b, kSurfaceB, 2 * kW, kH, kPitch);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff112233);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    setup_surface_sized(b, kChild, kW, kH / 2, kPitch);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff445566);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    // The large one read as a texture: the child's pixels where it lies (bottom left), the large one's beside it.
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFp2Offset, 0x40);
    bind_vertex_arrays(b);
    bind_linear_texture(b, kSurfaceB, tex::A8R8G8B8, kPitch);
    b.method(m::NV4097_SET_TEXTURE_IMAGE_RECT, ((2 * kW) << 16) | kH);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().surface_children == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 5, 60) == std::array<u8, 4>{0xff, 0x44, 0x55, 0x66});
    CHECK(guest_pixel(e, kSurfaceOffset, 58, 60) == std::array<u8, 4>{0xff, 0x11, 0x22, 0x33});
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kPitch * kH));
    CHECK(guest_pixel_pitch(e, kSurfaceB, kPitch, 5, 5) == std::array<u8, 4>{0xff, 0x11, 0x22, 0x33});
    CHECK(guest_pixel_pitch(e, kSurfaceB, kPitch, 5, kH / 2 + 5) == std::array<u8, 4>{0xff, 0x44, 0x55, 0x66});
    CHECK(guest_pixel_pitch(e, kSurfaceB, kPitch, kW + 5, kH / 2 + 5) == std::array<u8, 4>{0xff, 0x11, 0x22, 0x33});

    // The large one written over the child's left half; then the child drawn again (a corner of it): it takes its
    // place back from the large one first.
    auto c = e.at(b.offset());
    setup_surface_sized(c, kSurfaceB, 2 * kW, kH, kPitch);
    c.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {(kW / 2) << 16, kH << 16}); // x 0 to kW/2, every row
    c.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff778899);
    c.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    setup_surface_sized(c, kChild, kW, kH / 2, kPitch);
    c.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {(2u << 16) | (kW - 2), (2u << 16)}); // its top-right 2x2
    c.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xffaabbcc);
    c.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    CHECK(e.failures.empty());
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kPitch * kH));
    CHECK(guest_pixel_pitch(e, kSurfaceB, kPitch, 5, kH / 2 + 5) == std::array<u8, 4>{0xff, 0x77, 0x88, 0x99});
    CHECK(guest_pixel_pitch(e, kSurfaceB, kPitch, kW / 2 + 5, kH / 2 + 5) == std::array<u8, 4>{0xff, 0x44, 0x55, 0x66});
    CHECK(guest_pixel_pitch(e, kSurfaceB, kPitch, kW - 1, kH / 2) == std::array<u8, 4>{0xff, 0xaa, 0xbb, 0xcc});
    CHECK(guest_pixel_pitch(e, kSurfaceB, kPitch, 5, 5) == std::array<u8, 4>{0xff, 0x77, 0x88, 0x99});
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().readbacks == 0); // run 066: a write-back and a decode from memory every frame
    CHECK(e.backend->stats().color_conversions == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0x20, 0x80, 0x20, 0x80});
}

namespace {

// GTA V reads its 640x360 F_W16Z16Y16X16 target as a 1280x720 A8R8G8B8 texture. Here: a float16 target with a
// gradient drawn into it, then a pass that samples it as an A8R8G8B8 texture twice as wide and twice as tall (its lower
// half is the memory after the target, a pattern). Returns the second target's bytes; `readbacks`: the readbacks before
// they were flushed (the flush is one); `end`: where the commands end.
std::vector<u8> sample_f16_as_argb(E2E& e, u64& readbacks, u32* end = nullptr)
{
    e.put_local_bytes(kFpOffset, fp_color(false));
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0xff102030, 0x40ff8000, 0x2050ffff});
    for (u32 i = 0; i < kW * 8 * kH; i++) {
        ps_write8(e.mem.base(), PS_MEM_RSX_LOCAL_BASE + kSurfaceB + kW * 8 * kH + i, static_cast<u8>(i * 7 + 3));
    }
    auto b = e.at(0);
    setup_surface_as(b, kSurfaceB, SurfaceColorFormat::F_W16Z16Y16X16, kW * 8);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0x80402010);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    set_fragment_program(b, kFp2Offset, 0x40);
    bind_linear_texture(b, kSurfaceB, tex::A8R8G8B8, kW * 8, 2 * kW, 2 * kH);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    if (end) *end = b.offset();
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    readbacks = e.backend->stats().readbacks;
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    const u8* p = e.mem.host(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset);
    return {p, p + kW * kH * 4};
}

} // namespace

TEST_CASE("metal e2e: a float16 target read as a wider A8R8G8B8 texture is laid over memory on the GPU, as a "
          "write-back would give, without one (D-137)")
{
    REQUIRE_METAL();
    E2E laid;
    u64 readbacks = 0;
    const std::vector<u8> on_gpu = sample_f16_as_argb(laid, readbacks);
    CHECK(readbacks == 0);
    CHECK(laid.backend->stats().texture_overlays == 1);
    // The target's memory was not written: guest memory still has what was there before (zeros).
    CHECK(ps_read32(laid.mem.base(), PS_MEM_RSX_LOCAL_BASE + kSurfaceB + (40 * kW + 32) * 8) == 0);

    E2E written(0, {}, 1800, false);
    const std::vector<u8> from_memory = sample_f16_as_argb(written, readbacks);
    CHECK(readbacks == 1);
    CHECK(written.backend->stats().texture_overlays == 0);
    CHECK(on_gpu == from_memory);

    // Both halves were sampled: rows above the middle read the target's rows, the ones below the pattern after it.
    usize from_target = 0, from_pattern = 0;
    for (u32 y = 0; y < kH; y++) {
        for (u32 x = 0; x < kW; x++) {
            const u8* px = &on_gpu[(y * kW + x) * 4];
            if (px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 0) continue; // outside the triangle
            (y < kH / 2 ? from_target : from_pattern)++;
        }
    }
    CHECK(from_target > 100);
    CHECK(from_pattern > 100);
}

TEST_CASE("metal e2e: a texture laid over a target is decoded again after the target is drawn to again (D-137)")
{
    REQUIRE_METAL();
    E2E e;
    u32 end = 0;
    u64 readbacks = 0;
    sample_f16_as_argb(e, readbacks, &end);
    REQUIRE(e.backend->stats().texture_overlays == 1);
    // The same texture again, the target untouched: the cached decode.
    auto b = e.at(end);
    bind_linear_texture(b, kSurfaceB, tex::A8R8G8B8, kW * 8, 2 * kW, 2 * kH);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    CHECK(e.backend->stats().texture_overlays == 1);
    // The target cleared again, then sampled: laid over again.
    auto c = e.at(b.offset());
    setup_surface_as(c, kSurfaceB, SurfaceColorFormat::F_W16Z16Y16X16, kW * 8);
    c.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff00ff00);
    c.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    setup_surface(c, kSurfaceOffset);
    bind_linear_texture(c, kSurfaceB, tex::A8R8G8B8, kW * 8, 2 * kW, 2 * kH);
    draw_triangle(c);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().texture_overlays == 2);
    CHECK(e.backend->stats().readbacks == 1); // the first flush
    // Opaque green in float16 is 0000 3C00 0000 3C00 in memory (R G B A, big-endian): A8R8G8B8 texels 00 00 3C 00, each
    // alpha 0x00, red 0x00, green 0x3C, blue 0x00. Row 20 samples the texture's row 41: the target's.
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 20) == std::array<u8, 4>{0x00, 0x00, 0x3C, 0x00});
}

TEST_CASE("metal e2e: a texture a target covers whole comes from the target alone, no copy of memory (D-214)")
{
    // A depth buffer sampled through a texture format at its own row size every frame (Assassin's Creed, 2 a frame):
    // its texels all come from the buffer laid over, so nothing is copied from guest memory and nothing is watched.
    REQUIRE_METAL();
    constexpr u32 kDepth = 0x180000;
    E2E e;
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
    const auto clear_and_sample = [&](u32 at, u32 depth_value) {
        auto b = e.at(at);
        setup_surface_as(b, kSurfaceB, SurfaceColorFormat::A8R8G8B8, kW * 4, kDepth);
        b.method(m::NV4097_SET_STENCIL_MASK, 0xff);
        b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, depth_value);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kDepth | clear::kStencil);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFp2Offset, 0x40);
        bind_vertex_arrays(b);
        bind_linear_texture(b, kDepth, tex::R5G6B5, kW * 4, 2 * kW, kH); // two texels per depth word
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        return b.offset();
    };
    // Every texel the same R5G6B5 value: both halves of the depth word (depth 0xFFFFFF and stencil 0xFF).
    u32 at = clear_and_sample(0, 0xffffffff);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().textures_from_target == 1);
    CHECK(e.backend->stats().texture_bytes_copied == 0);
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == (std::array<u8, 4>{0xff, 0xff, 0xff, 0xff}));
    // Cleared again (zero): the texture follows the buffer, still without a copy.
    at = clear_and_sample(at, 0);
    CHECK(e.backend->stats().textures_from_target == 2);
    CHECK(e.backend->stats().texture_bytes_copied == 0);
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == (std::array<u8, 4>{0xff, 0x00, 0x00, 0x00}));
}

TEST_CASE("metal e2e: a texture over two depth buffers side by side shows both, from the GPU (D-214)")
{
    // Assassin's Creed samples four 1024x1024 depth buffers side by side (one row pitch) as one 4096x1024 texture.
    // Only the buffer at the texture's address was laid over; the others' part came from stale memory.
    REQUIRE_METAL();
    constexpr u32 kDepth = 0x180000, kPitch = 2 * kW * 4;
    E2E e;
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
    auto b = e.at(0);
    const auto clear_depth = [&](u32 colour, u32 depth, u32 value) {
        setup_surface_as(b, colour, SurfaceColorFormat::A8R8G8B8, kPitch, depth);
        b.method(m::NV4097_SET_SURFACE_PITCH_Z, kPitch);
        b.method(m::NV4097_SET_STENCIL_MASK, 0xff);
        b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, value);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kDepth | clear::kStencil);
    };
    clear_depth(kSurfaceB, kDepth, 0xffffffff);             // the left buffer: R5G6B5 0xFFFF, white
    clear_depth(kSurfaceB + kW * 4, kDepth + kW * 4, 0xf800f800u); // the right one, beside it: R5G6B5 0xF800, red
    e.put_local32(kDepth + kW * 4 + 40 * kPitch + 16, 0x12345678u); // memory there is stale (not what the GPU has)
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0x00808080);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFp2Offset, 0x40);
    bind_vertex_arrays(b);
    bind_linear_texture(b, kDepth, tex::R5G6B5, kPitch, 4 * kW, kH); // both buffers' rows, two texels per word
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().readbacks == 0);
    CHECK(e.backend->stats().textures_from_target == 1);
    CHECK(e.backend->stats().texture_bytes_copied == 0);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 16, 48) == (std::array<u8, 4>{0xff, 0xff, 0xff, 0xff})); // u 0.25: the left
    CHECK(guest_pixel(e, kSurfaceOffset, 48, 48) == (std::array<u8, 4>{0xff, 0xff, 0x00, 0x00})); // u 0.75: the right
}

TEST_CASE("metal e2e: a texture with its own layout inside a target's memory: not laid over; the target written back "
          "when it is live, left when it is old (D-214, D-1056)")
{
    // Run 132: a compressed texture inside a big depth buffer's memory. Only targets in the texture's own rows (the
    // same pitch) are laid over. A target of another layout under the texture that the GPU drew in the last two flips
    // wrote that memory last (as the RSX's writes reach memory): it is written back first (D-1056). One older than
    // that is taken as memory the game reused: the texture is memory alone, as before.
    REQUIRE_METAL();
    for (const int mode : {0, 1, 2}) { // live; old; live, with PSPOCKET_RSX_NO_STALE_SYNC (the alarm alone)
        const bool live = mode != 1, sync = mode != 2;
        INFO((live ? "drawn this frame" : "drawn three flips before") << (sync ? "" : ", no sync"));
        if (!sync) ::setenv("PSPOCKET_RSX_NO_STALE_SYNC", "1", 1);
        E2E e;
        ::unsetenv("PSPOCKET_RSX_NO_STALE_SYNC");
        e.put_local_bytes(kFp2Offset, fp_texture());
        write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
        auto b = e.at(0);
        setup_surface(b, kSurfaceB); // kW x kH, pitch kW * 4
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xffff0000);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0x00808080);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        if (!live)
            for (int i = 0; i < 3; i++) b.method(m::GCM_FLIP_COMMAND, 0);
        upload_vertex_program(b);
        set_fragment_program(b, kFp2Offset, 0x40);
        bind_vertex_arrays(b);
        bind_linear_texture(b, kSurfaceB + 4096, tex::A8R8G8B8, kW * 2, kW / 2, kH / 2); // half the pitch, inside it
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        CHECK(e.backend->stats().texture_overlays == 0);
        CHECK(e.backend->stats().textures_from_target == 0);
        CHECK(e.backend->stats().stale_synced == (live && sync ? 1u : 0u));
        CHECK(e.backend->stats().stale_left == (live ? 0u : 1u));
        CHECK(e.rsx->alarms().get(Alarm::StaleGpuRead) == (sync ? 0u : 1u));
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        if (live && sync) CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == (std::array<u8, 4>{0xff, 0xff, 0x00, 0x00})); // red
        else CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == (std::array<u8, 4>{0x00, 0x00, 0x00, 0x00})); // memory
    }
}

TEST_CASE("metal e2e: a Z24S8 depth buffer read as a texture of another size is laid over memory on the GPU, as a "
          "write-back would give (D-137)")
{
    REQUIRE_METAL();
    constexpr u32 kDepth = 0x180000;
    u64 readbacks = 0;
    const auto run = [&](E2E& e) {
        e.put_local_bytes(kFp2Offset, fp_texture());
        write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
        auto b = e.at(0);
        setup_surface_as(b, kSurfaceB, SurfaceColorFormat::A8R8G8B8, kW * 4, kDepth);
        b.method(m::NV4097_SET_STENCIL_MASK, 0xff);
        b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0x12345678);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kDepth | clear::kStencil);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFp2Offset, 0x40);
        bind_vertex_arrays(b);
        // R5G6B5 texels, two per depth word: a format no GPU conversion of a depth buffer gives (D-088)
        bind_linear_texture(b, kDepth, tex::R5G6B5, kW * 4, 2 * kW, kH);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        readbacks = e.backend->stats().readbacks;
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        const u8* p = e.mem.host(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset);
        return std::vector<u8>(p, p + kW * kH * 4);
    };
    E2E laid;
    const std::vector<u8> on_gpu = run(laid);
    CHECK(readbacks == 0);
    CHECK(laid.backend->stats().texture_overlays == 1);
    E2E written(0, {}, 1800, false);
    const std::vector<u8> from_memory = run(written);
    CHECK(readbacks == 1);
    CHECK(on_gpu == from_memory);
    // Texel 2n+1 of a row is the word's low half, 0x5678: R5G6B5 red 10, green 51, blue 24, expanded to 8 bits
    // (RPCS3's expansion); the even texels hold 0x1234 (red 2, green 17, blue 20).
    const auto px = guest_pixel(laid, kSurfaceOffset, 32, 48);
    const bool low = px == std::array<u8, 4>{0xff, 0x52, 0xCE, 0xC5};
    const bool high = px == std::array<u8, 4>{0xff, 0x10, 0x45, 0xA5};
    CHECK((low || high));
}

TEST_CASE("metal e2e: a G8B8 target read as a G8B8 texture samples it, (A, R, G, B) = (G, B, G, B), and a G8B8 texture "
          "in memory decodes the same")
{
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0x10208040, 0x10208040, 0x10208040}); // r 0x10, g 0x20, b 0x80
    auto b = e.at(0);
    // Pass 1: a G8B8 target keeps the shader's green and blue
    setup_surface_as(b, kSurfaceB, SurfaceColorFormat::G8B8, kW * 2);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    // Pass 2: read as a G8B8 texture
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    set_fragment_program(b, kFp2Offset, 0x40);
    bind_linear_texture(b, kSurfaceB, tex::G8B8, kW * 2);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().surface_samples == 1);
    CHECK(e.backend->stats().textures_decoded == 0);
    CHECK(e.backend->stats().readbacks == 0);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0x20, 0x80, 0x20, 0x80});
    // The target in memory: green, then blue.
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH * 2));
    const u8* t = e.mem.host(PS_MEM_RSX_LOCAL_BASE + kSurfaceB + (48 * kW + 32) * 2);
    CHECK(t[0] == 0x20);
    CHECK(t[1] == 0x80);

    // The same bytes as a texture in plain memory, decoded.
    for (u32 i = 0; i < kW * kH; i++) {
        ps_write8(e.mem.base(), PS_MEM_RSX_LOCAL_BASE + kTexOffset + 2 * i, 0x20);
        ps_write8(e.mem.base(), PS_MEM_RSX_LOCAL_BASE + kTexOffset + 2 * i + 1, 0x80);
    }
    auto c = e.at(b.offset());
    c.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    c.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    bind_linear_texture(c, kTexOffset, tex::G8B8, kW * 2);
    draw_triangle(c);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().textures_decoded == 1);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().readbacks == 0); // converted on the GPU (D-088)
    CHECK(e.backend->stats().depth_conversions == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0x12, 0x34, 0x56, 0x78});
}

TEST_CASE("metal e2e: draws that sample the target they draw to, without writing it, share one copy (D-148)")
{
    // GTA V samples a target of its own pass with that target's colour mask off (run 089). A copy per draw was up to
    // ~200 whole targets a frame, each ending the pass and held until the GPU ran the frame: 4.7 GB on the iPhone.
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff00ff00);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFp2Offset, 0x40);
    bind_vertex_arrays(b);
    bind_linear_texture(b, kSurfaceOffset, tex::A8R8G8B8, kW * 4);
    b.method(m::NV4097_SET_COLOR_MASK, 0);
    for (int i = 0; i < 50; i++) draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().feedback_copies == 1);
    CHECK(e.backend->stats().render_passes <= 2); // the clear's, then the draws' after the one copy
}

TEST_CASE("metal e2e: a draw that samples the target it draws to sees the draws before it after an invalidation (D-148)")
{
    // The copy is shared until the target is written and the game invalidates the texture cache (D-947): a draw after
    // both samples the write.
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0x00ff00ff, 0x00ff00ff, 0x00ff00ff}); // green
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    bind_vertex_arrays(b);
    bind_linear_texture(b, kSurfaceOffset, tex::A8R8G8B8, kW * 4);
    set_fragment_program(b, kFp2Offset, 0x40);
    b.method(m::NV4097_SET_COLOR_MASK, 0);
    draw_triangle(b); // samples the black clear (a copy), writes nothing
    set_fragment_program(b, kFpOffset, 0x40);
    b.method(m::NV4097_SET_COLOR_MASK, 0x01010101);
    draw_triangle(b); // writes green
    set_fragment_program(b, kFp2Offset, 0x40);
    b.method(m::NV4097_INVALIDATE_L2, 1); // cellGcmSetInvalidateTextureCache
    draw_triangle(b); // samples the green (a new copy) and writes it: black here would be the old copy
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().feedback_copies == 2);
    CHECK(e.backend->stats().feedback_invalidated == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00});
}

TEST_CASE("metal e2e: without an invalidation, a draw that samples its target reads it as the texture cache took it (D-947)")
{
    // GTA V's draws that read the target they write, up to 400 a frame on the iPhone (run 327): a copy and a render pass
    // each. On the RSX they read it through the texture cache, which sees later writes only after the game invalidates
    // it: the first draw's copy serves the ones after it, and they keep their pass.
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0x00ff00ff, 0x00ff00ff, 0x00ff00ff}); // green
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    bind_vertex_arrays(b);
    bind_linear_texture(b, kSurfaceOffset, tex::A8R8G8B8, kW * 4);
    set_fragment_program(b, kFp2Offset, 0x40);
    b.method(m::NV4097_SET_COLOR_MASK, 0);
    draw_triangle(b); // samples the black clear (a copy), writes nothing
    set_fragment_program(b, kFpOffset, 0x40);
    b.method(m::NV4097_SET_COLOR_MASK, 0x01010101);
    draw_triangle(b); // writes green
    set_fragment_program(b, kFp2Offset, 0x40);
    for (int i = 0; i < 20; i++) draw_triangle(b); // read the black the cache took in, and write it
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().feedback_copies == 1);
    CHECK(e.backend->stats().feedback_reused == 20);
    CHECK(e.backend->stats().render_passes <= 2); // the clear's, then the draws' after the one copy
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0x00, 0x00});
}

TEST_CASE("metal e2e: a target sampled again after the draws went to other targets is copied again (D-947)")
{
    // The copy stands for the texture cache over one run of draws to the same targets: draws to others in between end
    // it, as the RSX's small cache would have let go of the lines by then.
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    e.put_local_bytes(kFp2Offset, fp_texture());
    write_triangle(e, {0x00ff00ff, 0x00ff00ff, 0x00ff00ff}); // green
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    bind_vertex_arrays(b);
    bind_linear_texture(b, kSurfaceOffset, tex::A8R8G8B8, kW * 4);
    set_fragment_program(b, kFp2Offset, 0x40);
    b.method(m::NV4097_SET_COLOR_MASK, 0);
    draw_triangle(b); // samples the black clear (a copy), writes nothing
    set_fragment_program(b, kFpOffset, 0x40);
    b.method(m::NV4097_SET_COLOR_MASK, 0x01010101);
    draw_triangle(b); // writes green
    setup_surface(b, kSurfaceB); // a draw to another target
    draw_triangle(b);
    setup_surface(b, kSurfaceOffset); // and back
    set_fragment_program(b, kFp2Offset, 0x40);
    draw_triangle(b); // samples the green (a new copy): the targets changed since the first
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().feedback_copies == 2);
    CHECK(e.backend->stats().feedback_rebound == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00});
}

TEST_CASE("metal e2e: the signed blend equations are done in the shader (D-948)")
{
    // GTA V draws with NV4097_SET_BLEND_EQUATION ADD_SIGNED: the source byte, as two's complement, added to the target
    // (RPCS3 RSXProgrammableBlendPrologue.glsl). Drawn as Metal's add, a source of 0xf0 (-16) lit the target to white
    // where it darkens it (run 327: bright patches around the cars on the iPhone).
    REQUIRE_METAL();
    const auto run = [](u32 equation) {
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0xf0100000, 0xf0100000, 0xf0100000}); // r 0xf0 (-16), g 0x10 (+16), b 0, a 0
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff808080);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_BLEND_ENABLE, 1);
        b.method(m::NV4097_SET_BLEND_EQUATION, equation);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        CHECK(e.backend->stats().shader_blend_draws == 1);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        const auto px = guest_pixel(e, kSurfaceOffset, 32, 48);
        return std::array<int, 4>{px[0], px[1], px[2], px[3]}; // a r g b
    };
    const auto near = [](const std::array<int, 4>& got, const std::array<int, 4>& want) {
        for (usize i = 0; i < 4; i++) {
            if (std::abs(got[i] - want[i]) > 1) return false;
        }
        return true;
    };
    SECTION("ADD_SIGNED: the target plus the signed source")
    {
        const auto px = run(0xf006f006);
        INFO(px[0] << " " << px[1] << " " << px[2] << " " << px[3]);
        CHECK(near(px, {0xff, 0x70, 0x90, 0x80}));
    }
    SECTION("REVERSE_SUBTRACT_SIGNED: the target minus the signed source")
    {
        const auto px = run(0xf005f005);
        INFO(px[0] << " " << px[1] << " " << px[2] << " " << px[3]);
        CHECK(near(px, {0xff, 0x90, 0x70, 0x80}));
    }
    SECTION("ADD_SIGNED on a B8 target (GTA V's, run 327)")
    {
        // The B8 target keeps the shader's blue: 0x00 (0) added to the clear's 0x80. Blue 0xf0 here instead: -16.
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0x0000f000, 0x0000f000, 0x0000f000}); // b 0xf0 (-16)
        auto b = e.at(0);
        setup_surface_as(b, kSurfaceB, SurfaceColorFormat::B8, kW);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0x80808080);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_BLEND_ENABLE, 1);
        b.method(m::NV4097_SET_BLEND_EQUATION, 0xf006f006);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        CHECK(e.backend->stats().shader_blend_draws == 1);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH));
        const int v = *e.mem.host(PS_MEM_RSX_LOCAL_BASE + kSurfaceB + 48 * kW + 32);
        INFO(v);
        CHECK(std::abs(v - 0x70) <= 1);
    }
    SECTION("REVERSE_ADD_SIGNED: the source plus the signed target")
    {
        // The target's 0x80 is -128 as a signed byte and its 0xff -1: the source minus 128/255 (alpha minus 1/255),
        // clamped.
        const auto px = run(0xf007f007);
        INFO(px[0] << " " << px[1] << " " << px[2] << " " << px[3]);
        CHECK(near(px, {0x00, 0x70, 0x00, 0x00}));
    }
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    // texture coordinate's z) is 0.5. LEQUAL passes where depth <= 0.5: the texel is the comparison's left side, as RPCS3
    // reverses the texture's function for its comparison samplers, which put the reference there (GL
    // glutils/sampler.cpp, VKDraw.cpp get_compare_func(zfunc, true); D-949). The left half white, the right half black;
    // with linear filtering the 2x2 comparisons around the edge blend.
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
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        CHECK(guest_pixel(e, kSurfaceOffset, 20, 48) == std::array<u8, 4>{0xff, 0xff, 0xff, 0xff});
        CHECK(guest_pixel(e, kSurfaceOffset, 44, 48) == std::array<u8, 4>{0x00, 0x00, 0x00, 0x00});
        // Pixels 31 and 32 sample texel coordinates 15.25 and 15.75: between texels 15 (passes) and 16 (fails).
        const int left = guest_pixel(e, kSurfaceOffset, 31, 48)[1], right = guest_pixel(e, kSurfaceOffset, 32, 48)[1];
        if (linear) {
            CHECK(std::abs(left - 191) <= 4); // three quarters of texel 15
            CHECK(std::abs(right - 64) <= 4); // a quarter
        } else {
            CHECK(left == 0xff);
            CHECK(right == 0x00);
        }
    }
}

TEST_CASE("metal e2e: a shadow lookup on a depth buffer compares against the buffer itself (D-106)")
{
    // The depth buffer (Z16, 0.375) sampled directly as DEPTH16 with a compare function; the reference is the texture
    // coordinate's z, 0 here: the depth against it (D-949), LEQUAL fails (black), GREATER passes (white). No conversion,
    // no readback.
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
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        CHECK(e.backend->stats().depth_conversions == 0);
        CHECK(e.backend->stats().readbacks == 0);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        const u8 v = zfunc == 3 ? 0x00 : 0xff;
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
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    CHECK(e.rsx->status().conditional_skips == 0); // decided on the GPU (D-1058), the count known or not
    CHECK(e.backend->stats().predicated_draws == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceB, 32, 48) == std::array<u8, 4>{0, 0, 0, 0});
    auto d = e.at(c.offset());
    setup_surface(d, kSurfaceB);
    d.method(m::NV4097_SET_RENDER_ENABLE, (2u << 24) | 0x00);
    draw_triangle(d);
    d.method(m::NV4097_SET_RENDER_ENABLE, 1u << 24);
    REQUIRE(e.submit(d) == Rsx::RunStatus::Idle);
    CHECK(e.rsx->status().conditional_skips == 0);
    CHECK(e.backend->stats().predicated_draws == 2);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceB, 32, 48) != std::array<u8, 4>{0, 0, 0, 0});
}

TEST_CASE("metal e2e: a draw under a report the GPU has not counted yet is decided by the GPU on the count, as the "
          "exact mode decides it, whatever the older count (D-115, D-1058)")
{
    // On the iPhone, a query's count often arrived after the draw that tested it: an older zero skipped parts of
    // characters for a frame (D-115: such draws rendered whatever the count). Now the GPU decides once it has counted
    // (D-1058): the conditional draw goes to the GPU as an indirect draw whose instance count a kernel zeroes when the
    // query's counters sum to zero. The query counts a first triangle (visible, or scissored away); the second, under
    // it, shows only when the first counted. The exact mode (it waits for the count, D-1029) gives the same picture.
    REQUIRE_METAL();
    for (const bool exact : {false, true})
        for (const bool visible : {true, false}) {
            INFO((exact ? "exact" : "fast") << ", the query's draw " << (visible ? "visible" : "scissored away"));
            if (exact) ::setenv("PSPOCKET_RSX_EXACT", "1", 1);
            E2E e;
            ::unsetenv("PSPOCKET_RSX_EXACT");
            e.put_local_bytes(kFpOffset, fp_color(false));
            write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red
            ps_write32(e.mem.base(), e.config.layout.report_addr(1) + 8, visible ? 0 : 1); // an older count, wrong
            auto b = e.at(0);
            setup_surface(b, kSurfaceOffset);
            b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
            b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
            upload_vertex_program(b);
            set_fragment_program(b, kFpOffset, 0x40);
            bind_vertex_arrays(b);
            b.method(m::NV4097_SET_CONTEXT_DMA_REPORT, loc::kDmaReportLocal);
            b.method(m::NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE, 1);
            b.method(m::NV4097_CLEAR_REPORT_VALUE, reports::kZpassPixelCount);
            if (!visible) b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {0u << 16, 0u << 16}); // nothing passes
            draw_triangle(b); // the query's draw
            if (!visible) b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {kW << 16, kH << 16});
            b.method(m::NV4097_GET_REPORT, (reports::kZpassPixelCount << 24) | 0x10); // counted when the GPU runs it
            b.method(m::NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE, 0);
            b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
            b.method(m::NV4097_CLEAR_SURFACE, clear::kColor); // what the query's draw drew goes
            b.method(m::NV4097_SET_RENDER_ENABLE, (2u << 24) | 0x10);
            draw_triangle(b); // under it
            b.method(m::NV4097_SET_RENDER_ENABLE, 1u << 24);
            REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
            for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
            CHECK(e.failures.empty());
            // Fast: the GPU decided, the CPU skipped nothing; exact: the CPU decided on the count it waited for.
            const u32 skipped = exact && !visible ? 1u : 0u;
            CHECK(e.rsx->status().conditional_skips == skipped);
            CHECK(e.rsx->alarms().get(Alarm::ConditionalPending) == 0);
            if (!exact) CHECK(e.backend->stats().gpu_predicates == 1);
            if (!exact) CHECK(e.backend->stats().predicated_draws == 1);
            REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
            CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) ==
                  (visible ? std::array<u8, 4>{0xff, 0xff, 0x00, 0x00} : std::array<u8, 4>{0, 0, 0, 0}));
            // Once counted, the same test: the same answer (fast: on the GPU again, whether counted or not, so the
            // decision does not depend on the GPU's timing; exact: on the CPU).
            e.backend->sync();
            CHECK_FALSE(e.backend->report_pending(e.config.layout.report_addr(1) + 8));
            auto c = e.at(b.offset());
            c.method(m::NV4097_SET_RENDER_ENABLE, (2u << 24) | 0x10);
            draw_triangle(c);
            c.method(m::NV4097_SET_RENDER_ENABLE, 1u << 24);
            REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
            CHECK(e.rsx->status().conditional_skips == (exact && !visible ? 2u : 0u));
            if (!exact) CHECK(e.backend->stats().predicated_draws == 2);
            e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4); // nothing newer: exact
            e.backend->sync();
            CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) ==
                  (visible ? std::array<u8, 4>{0xff, 0xff, 0x00, 0x00} : std::array<u8, 4>{0, 0, 0, 0}));
        }
}

TEST_CASE("metal e2e: the draw log's ring holds each frame's draws (D-1036)")
{
    REQUIRE_METAL();
    setenv("PSPOCKET_RSX_DRAWLOG_RING", "2", 1);
    E2E e;
    unsetenv("PSPOCKET_RSX_DRAWLOG_RING");
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    for (int frame = 0; frame < 3; frame++) { // three frames of one draw each: the ring keeps the last two
        draw_triangle(b);
        b.method(m::GCM_FLIP_COMMAND, 0);
    }
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    const std::string path = (std::filesystem::temp_directory_path() / "pspocket-drawlog-test.txt").string();
    REQUIRE(e.backend->dump_drawlog(path));
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::filesystem::remove(path);
    INFO(text);
    CHECK(text.find("frame 1:") == std::string::npos);
    CHECK(text.find("frame 2: 1 draws") != std::string::npos);
    CHECK(text.find("frame 3: 1 draws") != std::string::npos);
    CHECK(text.find(std::format("-> {:#010x}", PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset)) != std::string::npos);
}

TEST_CASE("metal e2e: vertex arrays over a target read what the GPU drew there (render to vertex buffer, D-1034)")
{
    // GTA V's near water patch reads its heights and colours as vertex attributes from the X32 targets its water
    // simulation draws (local 0x2820000, 0x2830000). Copied from guest memory, the arrays held what the CPU last saw
    // there, and the patch went off the screen (the black block over the near sea, D-1031).
    REQUIRE_METAL();
    for (const bool overlay : {true, false}) { // laid over the copy on the GPU, or written back first
        INFO(std::format("overlay_targets {}", overlay));
        E2E e(0, {}, 1800, overlay);
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
        for (u32 v = 0; v < 3; v++) e.put_local32(kSurfaceB + v * 4, 0x00ff00ffu); // stale: green
        auto b = e.at(0);
        setup_surface(b, kSurfaceB); // the "simulation": its first pixels become the triangle's colours
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff00ff00u); // bytes ff 00 ff 00: r 255, g 0, b 255, a 0
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff202020u);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3, kSurfaceB); // colours from the target's memory
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        const auto mid = guest_pixel(e, kSurfaceOffset, 32, 40); // a r g b
        CHECK(mid[1] > 240);
        CHECK(mid[2] < 16);
        CHECK(mid[3] > 240);
        if (overlay) CHECK(e.backend->stats().vertex_overlays >= 1);
        if (!overlay) continue;
        // Read again over five flips with no new drawing into it: the reads keep the target live (D-1056's two flips
        // count from the last read; the water patch reads the simulation's targets on frames it does not draw them).
        auto c = e.at(0x8000);
        for (int flip = 0; flip < 5; flip++) {
            c.method(m::GCM_FLIP_COMMAND, 0);
            c.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff202020u);
            c.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
            draw_triangle(c);
        }
        REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        const auto later = guest_pixel(e, kSurfaceOffset, 32, 40);
        CHECK(later[1] > 240);
        CHECK(later[2] < 16);
        CHECK(e.backend->stats().vertex_overlays >= 6);
    }
}

TEST_CASE("metal e2e: a ZPASS report is ready only once the GPU has counted it, written whole (D-1021)")
{
    // GTA V's queries (0xB21908 begin: count 0, pad 1 by the CPU; 0xB21AD0 ready: pad 0) take a report whose pad is 0
    // as counted. Its water module skipped the sea on a "ready" zero that came frames before the count (2026-10-16).
    REQUIRE_METAL();
    // The host clock starts at its first read (a run's boot): read it here, so a timestamp the handler writes is past
    // 0 even when this test runs alone (it read 0 there one run in three).
    const u64 before = Rsx::timestamp_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    for (const bool early : {false, true}) {
        INFO(std::format("PSPOCKET_RSX_EARLY_REPORTS {}", early));
        if (early) setenv("PSPOCKET_RSX_EARLY_REPORTS", "1", 1);
        E2E e;
        unsetenv("PSPOCKET_RSX_EARLY_REPORTS");
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
        const u32 report = e.config.layout.report_addr(0);
        ps_write64(e.mem.base(), report, 0);
        ps_write32(e.mem.base(), report + 8, 0); // the game's begin: count 0, pad 1
        ps_write32(e.mem.base(), report + 12, 1);
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_CONTEXT_DMA_REPORT, loc::kDmaReportLocal);
        b.method(m::NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE, 1);
        b.method(m::NV4097_CLEAR_REPORT_VALUE, reports::kZpassPixelCount);
        draw_triangle(b);
        b.method(m::NV4097_GET_REPORT, (reports::kZpassPixelCount << 24) | 0x00);
        b.method(m::NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE, 0);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        // Nothing went to the GPU yet (no flip, no sync): the count is not there.
        CHECK(e.backend->report_pending(report + 8));
        CHECK(ps_read32(e.mem.base(), report + 8) == 0);
        if (!early) {
            CHECK(ps_read64(e.mem.base(), report) == 0);
            CHECK(ps_read32(e.mem.base(), report + 12) == 1); // not ready
            CHECK(e.rsx->alarms().get(Alarm::ReportEarly) == 0);
        } else {
            CHECK(ps_read64(e.mem.base(), report) != 0);
            CHECK(ps_read32(e.mem.base(), report + 12) == 0); // "ready" with the old zero: the race
            CHECK(e.rsx->alarms().get(Alarm::ReportEarly) == 1);
        }
        e.backend->sync();
        const u32 count = wait_report(e, 0, 0);
        CHECK(count > 1000);
        CHECK(ps_read32(e.mem.base(), report + 12) == 0);
        CHECK(ps_read64(e.mem.base(), report) > before); // the GPU's completion time, not the method's
        CHECK_FALSE(e.backend->report_pending(report + 8));
    }
}

TEST_CASE("metal e2e: occlusion queries past a command buffer's counters still count (D-1024)")
{
    // 4096 counters a command buffer: a query past them counted nothing, and a draw it guarded was skipped.
    REQUIRE_METAL();
    g_e2e_config = [](MetalBackendConfig& c) { c.occlusion_counters = 1; };
    E2E e;
    g_e2e_config = nullptr;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    for (u32 i = 0; i < 3; i++) ps_write32(e.mem.base(), e.config.layout.report_addr(i) + 8, 0xDEADBEEF);
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    b.method(m::NV4097_SET_CONTEXT_DMA_REPORT, loc::kDmaReportLocal);
    b.method(m::NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE, 1);
    for (u32 i = 0; i < 3; i++) {
        b.method(m::NV4097_CLEAR_REPORT_VALUE, reports::kZpassPixelCount);
        draw_triangle(b);
        b.method(m::NV4097_GET_REPORT, (reports::kZpassPixelCount << 24) | (i * 16));
    }
    b.method(m::NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE, 0);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    e.backend->sync();
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    const u32 first = wait_report(e, 0, 0xDEADBEEF);
    CHECK(first > 1000);
    CHECK(wait_report(e, 1, 0xDEADBEEF) == first);
    CHECK(wait_report(e, 2, 0xDEADBEEF) == first);
    CHECK(e.rsx->alarms().get(Alarm::OcclusionOverflow) >= 1);
}

TEST_CASE("metal e2e: a depth buffer made over a colour target holds the target's bytes (D-1025)")
{
    // GTA V packs its depth into an A8B8G8R8 640x360 target (#2787), then tests against that memory as a Z24S8 buffer
    // (#2806+): a new depth buffer cleared to 1.0 lost what the target held (the striped sky of 2026-10-16).
    REQUIRE_METAL();
    for (const bool carry : {true, false}) {
        INFO(std::format("carry {}", carry));
        if (!carry) setenv("PSPOCKET_RSX_NO_CARRY", "1", 1);
        E2E e;
        unsetenv("PSPOCKET_RSX_NO_CARRY");
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0x11223344); // a r g b in memory: Z24S8 depth 0x112233, stencil 0x44
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        setup_surface(b, kSurfaceB);
        b.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
        b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, kSurfaceOffset);
        b.method(m::NV4097_SET_SURFACE_PITCH_Z, kW * 4);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_DEPTH_TEST_ENABLE, 1);
        b.method(m::NV4097_SET_DEPTH_FUNC, static_cast<u32>(CompareFunc::Always));
        b.method(m::NV4097_SET_DEPTH_MASK, 0);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        const u32 word = ps_read32(e.mem.base(), PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset + 5 * kW * 4 + 7 * 4);
        if (carry) {
            CHECK(word == 0x11223344);
            CHECK(e.backend->stats().targets_carried == 1);
            CHECK(e.rsx->alarms().get(Alarm::TargetLost) == 0);
        } else {
            CHECK(word == 0xFFFFFF00); // cleared to 1.0: the bytes lost
            CHECK(e.rsx->alarms().get(Alarm::TargetLost) == 1);
        }
    }
}

TEST_CASE("metal e2e: a colour target over a smaller one of another format carries its bytes, the rest from memory (D-1025)")
{
    // GTA V's 0xcd260000: an F_W16Z16Y16X16 320x180 target, then an A8B8G8R8 640x360 one with the same pitch over it.
    REQUIRE_METAL();
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    constexpr u32 kPitch = kW * 8; // the F16 target's rows, and as many bytes as 2 kW A8R8G8B8 pixels
    for (u32 y = 0; y < kH; y++)
        for (u32 x = 0; x < 2 * kW; x++) ps_write32(e.mem.base(), PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset + y * kPitch + x * 4, 0x01020304);
    auto b = e.at(0);
    setup_surface_sized(b, kSurfaceOffset, kW, kH / 2, kPitch);
    b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(SurfaceColorFormat::F_W16Z16Y16X16) | (2u << 5) | (1u << 8));
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xffffffff);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    // The A8R8G8B8 target over it, a draw that writes nothing (colour mask 0).
    setup_surface_sized(b, kSurfaceOffset, 2 * kW, kH, kPitch);
    b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (2u << 5) | (1u << 8));
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    b.method(m::NV4097_SET_COLOR_MASK, 0);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().targets_carried == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kPitch * kH));
    const auto word = [&](u32 x, u32 y) { return ps_read32(e.mem.base(), PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset + y * kPitch + x * 4); };
    CHECK(word(3, 2) == 0x3C003C00);          // the F16 target's 1.0s, as its bytes
    CHECK(word(2 * kW - 1, kH / 2 - 1) == 0x3C003C00);
    CHECK(word(3, kH / 2) == 0x01020304);     // below it: what memory held
    CHECK(e.rsx->alarms().get(Alarm::TargetLost) == 0);
}

TEST_CASE("metal e2e: a target nobody drew yet is sampled as guest memory holds it, not its pooled texture (D-1023)")
{
    // A target made for draws that were all skipped (their programs compiling, D-158) took a removed target's texture
    // from the pool (D-984): sampled, it showed that one's pixels.
    REQUIRE_METAL();
    {
        E2E e(0, {}, 1800, true, /*async_wait_ms=*/0.0, /*shader_interpreter=*/false);
        e.put_local_bytes(kFpOffset, fp_color(false));
        e.put_local_bytes(kFp2Offset, fp_texture());
        write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
        for (u32 i = 0; i < kW * kH; i++) ps_write32(e.mem.base(), PS_MEM_RSX_LOCAL_BASE + kTexOffset + i * 4, 0x01020304);
        auto b = e.at(0);
        setup_surface(b, kSurfaceB);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xffff0000);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        bind_vertex_arrays(b);
        // A B8 target over it (the A8R8G8B8 one goes to the pool), then an A8R8G8B8 one of its size at kTexOffset: both
        // for draws skipped while their programs compile.
        setup_surface_as(b, kSurfaceB, SurfaceColorFormat::B8, kW * 4);
        set_fragment_program(b, kFpOffset, 0x40);
        draw_triangle(b);
        setup_surface(b, kTexOffset);
        set_fragment_program(b, kFp2Offset, 0x40);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        e.backend->wait_for_async_compiles();
        // Sampled into kSurfaceOffset (again while its pipeline still compiles).
        u32 at = b.offset();
        for (int tries = 0; tries < 4; tries++) {
            const u64 compiling = e.backend->stats().draws_compiling;
            auto c = e.at(at);
            setup_surface(c, kSurfaceOffset);
            c.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
            c.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
            set_fragment_program(c, kFp2Offset, 0x40);
            bind_linear_texture(c, kTexOffset, tex::A8R8G8B8, kW * 4);
            draw_triangle(c);
            REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
            at = c.offset();
            if (e.backend->stats().draws_compiling == compiling) break;
            e.backend->wait_for_async_compiles();
        }
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        CHECK(e.backend->stats().draws_compiling >= 2);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0x01, 0x02, 0x03, 0x04});
        CHECK(e.rsx->alarms().get(Alarm::UninitSampled) == 0);
    }
}

TEST_CASE("metal e2e: a draw with a signed blend equation waits for its program, not drawn by the interpreter (D-1026)")
{
    // The interpreter (D-940) blends as Metal does: ADD_SIGNED drawn as add while GTA V's programs compiled (the
    // bench's skyfall run, an RSX failure each time). The draw is made again until its programs are there (a loaded
    // Mac compiles slowly): never by the interpreter, then by the translation, blending in its shader.
    REQUIRE_METAL();
    E2E e(0, {}, 1800, true, /*async_wait_ms=*/0.0, /*shader_interpreter=*/true);
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xf0100000, 0xf0100000, 0xf0100000});
    u32 at = 0;
    for (int tries = 0; tries < 20 && e.backend->stats().shader_blend_draws == 0; tries++) {
        auto b = e.at(at);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff808080);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_BLEND_ENABLE, 1);
        b.method(m::NV4097_SET_BLEND_EQUATION, 0xf006f006);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        at = b.offset();
        e.backend->wait_for_async_compiles();
    }
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().draws_interpreted == 0);
    CHECK(e.backend->stats().draws_compiling >= 1);  // the first draw waited
    CHECK(e.backend->stats().shader_blend_draws == 1); // then the translation drew it
}

TEST_CASE("metal e2e: the exact mode has what the GPU drew in guest memory when a label lands (D-1029)")
{
    // The reference mode: a CPU that sees a label sees what the RSX drew before it, as on the PS3 (GTA V's CPU reads
    // targets after its labels); a depth buffer made over a colour target starts with its bytes from memory.
    REQUIRE_METAL();
    setenv("PSPOCKET_RSX_EXACT", "1", 1);
    E2E e;
    unsetenv("PSPOCKET_RSX_EXACT");
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
    b.method(m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, loc::kDmaSemaphoreRW);
    b.method(m::NV406E_SEMAPHORE_OFFSET, 64 * 0x10);
    b.method(m::NV406E_SEMAPHORE_RELEASE, 1);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(ps_read32(e.mem.base(), e.config.layout.label_addr(64)) == 1);
    // No flush: memory has it already.
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00}); // the triangle
    CHECK(guest_pixel(e, kSurfaceOffset, 2, 2) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff});   // the clear
    CHECK(e.backend->stats().readbacks == 0); // on the GPU, into memory

    // A depth buffer over it holds its bytes: 0xff0000ff as Z24S8 is depth 0xff0000, stencil 0xff.
    auto c = e.at(b.offset());
    setup_surface(c, kSurfaceB);
    c.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
    c.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, kSurfaceOffset);
    c.method(m::NV4097_SET_SURFACE_PITCH_Z, kW * 4);
    c.method(m::NV4097_SET_DEPTH_TEST_ENABLE, 1);
    c.method(m::NV4097_SET_DEPTH_FUNC, static_cast<u32>(CompareFunc::Always));
    c.method(m::NV4097_SET_DEPTH_MASK, 0);
    draw_triangle(c);
    c.method(m::NV406E_SEMAPHORE_RELEASE, 2);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    CHECK(e.failures.empty());
    CHECK(ps_read32(e.mem.base(), e.config.layout.label_addr(64)) == 2);
    CHECK(ps_read32(e.mem.base(), PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset + 2 * kW * 4 + 2 * 4) == 0xff0000ff);
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
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
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
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().draws == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x20, 0x40, 0x80});
}

TEST_CASE("metal e2e: a vertex program samples cube and 3D vertex textures with xyz (D-274)")
{
    REQUIRE_METAL();
    // o[1] = TXL(c[0], TEX0): the colour at direction or coordinate c[0] of a linear A8R8G8B8 texture, one texel a
    // cube face (+X red, -X green, +Y blue, -Y white, +Z yellow, -Z cyan) or a 1x1x2 volume (slice 0 red, slice 1 green).
    const auto draw = [](bool cube, std::array<float, 4> at) {
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0, 0, 0});
        const u32 texels[6] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff, 0xffffff00, 0xff00ffff}; // A R G B
        for (u32 i = 0; i < 6; i++) e.put_local32(kTexOffset + i * 4, texels[i]);
        VpInst pos;
        pos.vec_op = 1;
        pos.src[0] = vp_in();
        pos.input = 0;
        pos.out = 0;
        pos.vec_mask = 0xf;
        VpInst txl;
        txl.vec_op = 0x19;
        txl.src[0] = vp_const();
        txl.constant = 0;
        txl.src[1] = vp_temp(0); // tex_num 0
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
        b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, 1u << 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT, {f2u(at[0]), f2u(at[1]), f2u(at[2]), f2u(at[3])});
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_VERTEX_TEXTURE_OFFSET, {
            kTexOffset,
            (loc::kLocal + 1) | (cube ? 1u << 2 : 0u) | ((cube ? 2u : 3u) << 4) | ((tex::A8R8G8B8 | tex::kLinearFlag) << 8) |
                (1u << 16),
            3u | (3u << 8) | (3u << 16),
            (1u << 31),
            4u | (cube ? 0u : 2u << 20), // CONTROL3: pitch, and the depth of a volume
            (1u << 16) | (1u << 24),     // nearest
            (1u << 16) | 1u,             // 1x1
        });
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        return guest_pixel(e, kSurfaceOffset, 32, 48);
    };
    CHECK(draw(true, {1.f, 0.f, 0.f, 0.f}) == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00});  // +X: red
    CHECK(draw(true, {0.f, 1.f, 0.f, 0.f}) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff});  // +Y: blue
    CHECK(draw(true, {0.f, 0.f, -1.f, 0.f}) == std::array<u8, 4>{0xff, 0x00, 0xff, 0xff}); // -Z: cyan
    CHECK(draw(false, {0.5f, 0.5f, 0.25f, 0.f}) == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00}); // slice 0: red
    CHECK(draw(false, {0.5f, 0.5f, 0.75f, 0.f}) == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00}); // slice 1: green
}

TEST_CASE("metal e2e: a texture unused for a while is freed, and decoded again when used again (D-116)")
{
    REQUIRE_METAL();
    E2E e(0, {}, 4);
    e.put_local_bytes(kFpOffset, fp_texture());
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    for (u32 i = 0; i < kW * kH; i++) e.put_local32(kTexOffset + i * 4, 0xff0000ffu); // blue
    e.rsx->set_display_buffer(0, {kSurfaceOffset, kW * 4, kW, kH});
    const auto draw = [&](CommandBuilder& b) {
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        bind_linear_texture(b, kTexOffset, tex::A8R8G8B8, kW * 4);
        draw_triangle(b);
    };
    auto b = e.at(0);
    draw(b);
    for (int i = 0; i < 10; i++) b.method(m::GCM_FLIP_COMMAND, 0); // ten flips without the texture after the first
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    CHECK(e.backend->stats().textures_decoded == 1);
    CHECK(e.backend->stats().textures_evicted == 1);
    auto c = e.at(b.offset());
    draw(c);
    REQUIRE(e.submit(c) == Rsx::RunStatus::Idle);
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().textures_decoded == 2); // decoded again
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
    CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff}); // the texture's blue
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

TEST_CASE("metal e2e: every flip gives a 64x36 thumbnail from the GPU, the image's box means (D-1027)")
{
    REQUIRE_METAL();
    using metal::kThumbnailWidth, metal::kThumbnailHeight;
    std::mutex mutex;
    std::vector<std::pair<u64, std::vector<u8>>> thumbs;
    g_e2e_config = [&](MetalBackendConfig& c) {
        c.thumbnail = [&](const u8* bgra, u64 flip) {
            std::lock_guard lock(mutex);
            thumbs.emplace_back(flip, std::vector<u8>(bgra, bgra + kThumbnailWidth * kThumbnailHeight * 4));
        };
    };
    E2E e(1);
    g_e2e_config = nullptr;
    e.put_local_bytes(kFpOffset, fp_color(false));
    write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
    e.rsx->set_display_buffer(0, {kSurfaceOffset, kW * 4, kW, kH});
    auto b = e.at(0);
    setup_surface(b, kSurfaceOffset);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    for (u32 color : {0xff00ff00u, 0xff203040u}) {
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, color);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        draw_triangle(b);
        b.method(m::GCM_FLIP_COMMAND, 0);
    }
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    e.backend->sync();
    std::lock_guard lock(mutex);
    std::lock_guard lock2(e.capture_mutex);
    REQUIRE(thumbs.size() == 2);
    REQUIRE(e.captures.size() == 2);
    for (usize f = 0; f < 2; f++) {
        CHECK(thumbs[f].first == f + 1);
        // The CPU's box filter of the whole image (pspocket-boot's compare_frame before D-1027).
        const std::vector<u8>& img = e.captures[f].second;
        const u32 w = 128, h = 128;
        u32 differ = 0;
        for (u32 ty = 0; ty < kThumbnailHeight; ty++) {
            const u32 y0 = ty * h / kThumbnailHeight, y1 = std::max(y0 + 1, (ty + 1) * h / kThumbnailHeight);
            for (u32 tx = 0; tx < kThumbnailWidth; tx++) {
                const u32 x0 = tx * w / kThumbnailWidth, x1 = std::max(x0 + 1, (tx + 1) * w / kThumbnailWidth);
                for (u32 c = 0; c < 3; c++) {
                    u64 sum = 0;
                    for (u32 y = y0; y < y1; y++)
                        for (u32 x = x0; x < x1; x++) sum += img[(y * w + x) * 4 + c];
                    differ += thumbs[f].second[(ty * kThumbnailWidth + tx) * 4 + c] != sum / (u64{y1 - y0} * (x1 - x0));
                }
            }
        }
        CHECK(differ == 0);
    }
    CHECK(thumbs[1].second[0] == 0x40); // the second clear's blue, top-left
}

// ---- D-174: fragment program ops and texture controls, run on the GPU -------------------------------------------

namespace {

std::string failure_text(const E2E& e)
{
    std::string s;
    for (const auto& f : e.failures) s += f.message + "\n";
    return s;
}

// Fragment program `prog` (32-bit exports: R0 is the colour) drawn over the triangle into a kW x kH F_W32Z32Y32X32
// target: pixel (32, 48) as floats. `setup` adds commands before the draw (textures, a vertex program of its own).
std::array<float, 4> fp_pixel(const std::vector<FpInst>& prog, const std::function<void(E2E&, CommandBuilder&)>& setup = {})
{
    E2E e;
    e.put_local_bytes(kFpOffset, assemble_fp(prog));
    write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
    auto b = e.at(0);
    setup_surface_as(b, kSurfaceOffset, SurfaceColorFormat::F_W32Z32Y32X32, kW * 16);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    if (setup) setup(e, b);
    draw_triangle(b);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    INFO(failure_text(e));
    CHECK(e.failures.empty());
    CHECK(e.backend->stats().draws == 1);
    REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 16));
    const u8* p = e.mem.host(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset + (48 * kW + 32) * 16);
    return {ps_ldf32(p), ps_ldf32(p + 4), ps_ldf32(p + 8), ps_ldf32(p + 12)};
}

bool near(const std::array<float, 4>& a, const std::array<float, 4>& b, float tolerance = 1e-5f)
{
    for (u32 i = 0; i < 4; i++) {
        if (!(std::abs(a[i] - b[i]) <= tolerance * std::max(1.f, std::abs(b[i])))) return false;
    }
    return true;
}

std::string show(const std::array<float, 4>& v) { return std::format("({}, {}, {}, {})", v[0], v[1], v[2], v[3]); }

#define CHECK_NEAR4(a, b)                                                                                               \
    do {                                                                                                                \
        const auto got_ = (a);                                                                                          \
        const std::array<float, 4> want_ = b;                                                                           \
        INFO("got " << show(got_) << ", want " << show(want_));                                                         \
        CHECK(near(got_, want_));                                                                                       \
    } while (0)

// MOV R<dst>, {k}
FpInst fp_mov_const(u32 dst, std::array<float, 4> k)
{
    FpInst in;
    in.op = 0x01;
    in.dst = dst;
    in.src[0] = fp_const();
    in.constant = k;
    return in;
}

// OP R0, {k}, R1, R2 (with the end bit)
FpInst fp_op_const(u32 op, std::array<float, 4> k)
{
    FpInst in;
    in.op = op;
    in.dst = 0;
    in.src[0] = fp_const();
    in.constant = k;
    in.src[1] = fp_r(1);
    in.src[2] = fp_r(2);
    in.end = true;
    return in;
}

// RPCS3 RSXFragmentPrologue.glsl srgb_to_linear, on the CPU.
float srgb_to_linear(float c) { return c > 0.04045f ? std::pow((c + 0.055f) / 1.055f, 2.4f) : c / 12.92f; }

} // namespace

TEST_CASE("metal e2e: a fragment program written again draws its new constants, and new instructions (D-210)")
{
    // Games write a fragment program's embedded constants in place between draws: the backend keeps the decode when
    // only constants changed, and decodes again when an instruction did.
    REQUIRE_METAL();
    E2E e;
    write_triangle(e, {0x0000ffffu, 0x0000ffffu, 0x0000ffffu}); // vertex colour (RGBA): blue
    const auto with_constant = [](std::array<float, 4> k) {
        FpInst in = fp_mov_const(0, k);
        in.end = true;
        return assemble_fp({in});
    };
    const std::vector<u8> programs[] = {with_constant({1.f, 0.f, 0.f, 1.f}), with_constant({0.f, 1.f, 0.f, 1.f}),
        fp_color(false), with_constant({1.f, 0.f, 0.f, 1.f})};
    u32 offset = 0;
    for (u32 i = 0; i < 4; i++) {
        e.put_local_bytes(kFpOffset, programs[i]);
        auto b = e.at(offset);
        setup_surface(b, kSurfaceOffset + i * 0x100000);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        offset = b.offset();
    }
    for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
    CHECK(e.failures.empty());
    const std::array<u8, 4> red{0xff, 0xff, 0x00, 0x00}, green{0xff, 0x00, 0xff, 0x00}, blue{0xff, 0x00, 0x00, 0xff};
    const std::array<u8, 4> expected[] = {red, green, blue, red};
    for (u32 i = 0; i < 4; i++) {
        INFO("draw " << i);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset + i * 0x100000, kW * kH * 4));
        CHECK(guest_pixel(e, kSurfaceOffset + i * 0x100000, 32, 48) == expected[i]);
    }
}

TEST_CASE("metal e2e: LIT, LIF, DST, REFL, DP2A and BEM compute what RPCS3 computes (D-174)")
{
    REQUIRE_METAL();
    // LIT (RPCS3 _builtin_lit): (1, max(x, 0), x > 0 ? max(y, 0) ^ w : 0, 1), y kept above 0 so that 0 ^ 0 is 1.
    CHECK_NEAR4(fp_pixel({fp_op_const(0x1E, {0.5f, 0.25f, 7.f, 2.f})}), (std::array<float, 4>{1.f, 0.5f, 0.0625f, 1.f}));
    CHECK_NEAR4(fp_pixel({fp_op_const(0x1E, {-1.f, 0.5f, 7.f, 2.f})}), (std::array<float, 4>{1.f, 0.f, 0.f, 1.f}));
    CHECK_NEAR4(fp_pixel({fp_op_const(0x1E, {0.5f, 0.f, 7.f, 0.f})}), (std::array<float, 4>{1.f, 0.5f, 1.f, 1.f}));
    // LIF (RPCS3 _builtin_lif): (1, y, y > 0 ? 2 ^ w : 0, 1), nothing clamped.
    CHECK_NEAR4(fp_pixel({fp_op_const(0x3C, {9.f, 0.5f, 9.f, 3.f})}), (std::array<float, 4>{1.f, 0.5f, 8.f, 1.f}));
    CHECK_NEAR4(fp_pixel({fp_op_const(0x3C, {9.f, -0.5f, 9.f, 3.f})}), (std::array<float, 4>{1.f, -0.5f, 0.f, 1.f}));
    // DST: (1, a.y * b.y, a.z, b.w)
    CHECK_NEAR4(fp_pixel({fp_mov_const(1, {9.f, 5.f, 9.f, 7.f}), fp_op_const(0x07, {9.f, 2.f, 3.f, 9.f})}),
        (std::array<float, 4>{1.f, 10.f, 3.f, 7.f}));
    // REFL (RPCS3: GLSL reflect(src0, src1) = src0 - 2 dot(src1, src0) src1): (1, -1) about the unnormalised (0, 2) is
    // (1, 7). NV_fragment_program's RFL (src1 reflected about src0) would give (-2, 0).
    CHECK_NEAR4(fp_pixel({fp_mov_const(1, {0.f, 2.f, 0.f, 0.f}), fp_op_const(0x36, {1.f, -1.f, 0.f, 0.f})}),
        (std::array<float, 4>{1.f, 7.f, 0.f, 0.f}));
    // DP2A: dot(a.xy, b.xy) + c.x everywhere: 1 * 3 + 2 * 4 + 5
    CHECK_NEAR4(fp_pixel({fp_mov_const(1, {3.f, 4.f, 0.f, 0.f}), fp_mov_const(2, {5.f, 6.f, 7.f, 8.f}),
                    fp_op_const(0x2E, {1.f, 2.f, 0.f, 0.f})}),
        (std::array<float, 4>{16.f, 16.f, 16.f, 16.f}));
    // BEM (NV_fragment_program X2D): a.xy + [[m.x m.y] [m.z m.w]] * b.xy, twice
    CHECK_NEAR4(fp_pixel({fp_mov_const(1, {1.f, 2.f, 0.f, 0.f}), fp_mov_const(2, {0.5f, 0.25f, 2.f, 4.f}),
                    fp_op_const(0x2B, {0.1f, 0.2f, 0.f, 0.f})}),
        (std::array<float, 4>{1.1f, 10.2f, 1.1f, 10.2f}));
}

TEST_CASE("metal e2e: PKG packs four bytes through sRGB, UPG unpacks them back to linear (D-174)")
{
    REQUIRE_METAL();
    // Bytes 0x20, 0x80, 0xC0, 0x40 (x lowest): the word 0x40C08020, a normal float, so it reads back as it is.
    const std::array<u32, 4> bytes{0x20, 0x80, 0xC0, 0x40};
    std::array<float, 4> linear{};
    for (u32 i = 0; i < 4; i++) linear[i] = srgb_to_linear(static_cast<float>(bytes[i]) / 255.f);
    const auto packed = fp_pixel({fp_op_const(0x2C, linear)}); // PKG R0, {linear}: linear_to_srgb, then the bytes
    for (u32 i = 0; i < 4; i++) {
        INFO(std::format("component {}: {:#010x}", i, std::bit_cast<u32>(packed[i])));
        CHECK(std::bit_cast<u32>(packed[i]) == 0x40C08020u);
    }
    // UPG R0, {the word}: the bytes, srgb_to_linear on all four.
    CHECK_NEAR4(fp_pixel({fp_op_const(0x2D, {std::bit_cast<float>(0x40C08020u), 0.f, 0.f, 0.f})}), linear);
}

TEST_CASE("metal e2e: fragment program loops run their count, and their index picks texture coordinates (D-174)")
{
    REQUIRE_METAL();
    // 0x00: LOOP/REP, end 0x30    0x10: ADD R0, R0, {1, 1, 1, 1}    0x30: MOV R0, R0 END
    const auto count_runs = [](u32 op, u32 count, u32 init, s32 inc, bool never = false) {
        FpInst loop;
        loop.op = op;
        fp_loop(loop, count, init, inc, 0x30);
        if (never) loop.exec_lt = loop.exec_eq = loop.exec_gt = false;
        FpInst add;
        add.op = 0x03;
        add.src[0] = fp_r(0);
        add.src[1] = fp_const();
        add.constant = std::array<float, 4>{1.f, 1.f, 1.f, 1.f};
        FpInst last;
        last.op = 0x01;
        last.src[0] = fp_r(0);
        last.end = true;
        return fp_pixel({loop, add, last})[0];
    };
    CHECK(count_runs(0x43, 3, 2, 1) == 3.f);         // LOOP {3, 2, 1}: three runs (before D-174: from 2 up to 3, one)
    CHECK(count_runs(0x44, 4, 4, 4) == 4.f);         // REP with every field 4, as nouveau encodes it (before: none)
    CHECK(count_runs(0x43, 0, 0, 1) == 0.f);         // a count of 0 skips the body
    CHECK(count_runs(0x43, 3, 0, 1, true) == 0.f);   // so does a condition that never holds (before: once)

    // f[TEX(A0.x + n)] in LOOP {count, init, inc}: the vertex program writes TEX0-TEX3 = c[0..3], c[k].x = 2^k, so the
    // sum's x names the coordinates read. TEX9 (o[6]) is left (0, 0, 0, 1), and an index past TEX9 reads (0, 0, 0, 1).
    const auto indexed_sum = [](u32 count, u32 init, s32 inc, u32 offset) {
        FpInst loop;
        loop.op = 0x43;
        fp_loop(loop, count, init, inc, 0x20);
        FpInst add;
        add.op = 0x03;
        add.src[0] = fp_r(0);
        add.src[1] = fp_in();
        fp_indexed(add, offset);
        FpInst last;
        last.op = 0x01;
        last.src[0] = fp_r(0);
        last.end = true;
        return fp_pixel({loop, add, last}, [](E2E&, CommandBuilder& b) {
            const auto mov = [](u32 out, VpSrc src, u32 constant) {
                VpInst in;
                in.vec_op = 1;
                in.src[0] = src;
                in.constant = constant;
                in.out = out;
                in.vec_mask = 0xf;
                return in;
            };
            std::vector<VpInst> vp{mov(0, vp_in(), 0)};
            for (u32 k = 0; k < 4; k++) vp.push_back(mov(7 + k, vp_const(), k));
            vp.back().end = true;
            b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
            b.method(m::NV4097_SET_TRANSFORM_PROGRAM, assemble_vp(vp));
            b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
            b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, 1u << 0);
            b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
            std::vector<u32> c;
            for (u32 k = 0; k < 4; k++) {
                for (const float f : {static_cast<float>(1u << k), 0.f, 0.f, 0.f}) c.push_back(f2u(f));
            }
            b.method(m::NV4097_SET_TRANSFORM_CONSTANT, c);
        });
    };
    CHECK_NEAR4(indexed_sum(3, 1, 1, 0), (std::array<float, 4>{14.f, 0.f, 0.f, 0.f})); // TEX1 + TEX2 + TEX3
    CHECK_NEAR4(indexed_sum(2, 0, 2, 1), (std::array<float, 4>{10.f, 0.f, 0.f, 0.f})); // TEX1 + TEX3
    CHECK_NEAR4(indexed_sum(2, 3, -1, 0), (std::array<float, 4>{12.f, 0.f, 0.f, 0.f})); // TEX3 + TEX2
    CHECK_NEAR4(indexed_sum(2, 9, 1, 0), (std::array<float, 4>{0.f, 0.f, 0.f, 2.f}));   // TEX9 + past it
}

TEST_CASE("metal e2e: TEXBEM samples at BEM's transform of its coordinate, TXPBEM at that divided by its w (D-174)")
{
    REQUIRE_METAL();
    // A 4x4 linear A8R8G8B8 texture, texel (x, y) = (A, R, G, B) (0xff, 64x, 64y, 0x80), nearest, clamped.
    const auto setup = [](E2E& e, CommandBuilder& b) {
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++) e.put_local32(kTexOffset + (y * 4 + x) * 4, 0xff000080u | (64 * x) << 16 | (64 * y) << 8);
        bind_linear_texture(b, kTexOffset, tex::A8R8G8B8, 16, 4, 4);
    };
    // x2d = (0.125, 0.125) + [[0.25 0.25] [0.125 0.125]] * (1, 1) = (0.625, 0.375): texel (2, 1).
    const auto prog = [](u32 op) {
        FpInst in;
        in.op = op;
        in.src[0] = fp_r(1);
        in.src[1] = fp_r(2);
        in.src[2] = fp_r(3);
        in.end = true;
        return std::vector<FpInst>{fp_mov_const(1, {0.125f, 0.125f, 0.f, 0.f}), fp_mov_const(2, {1.f, 1.f, 0.f, 0.f}),
            fp_mov_const(3, {0.25f, 0.25f, 0.125f, 0.125f}), in};
    };
    CHECK_NEAR4(fp_pixel(prog(0x33), setup), (std::array<float, 4>{128.f / 255.f, 64.f / 255.f, 128.f / 255.f, 1.f}));
    // TXPBEM divides x2d.xy by its w, BEM's second row (RPCS3; untested there): (1.67, 1) clamps to texel (3, 3).
    CHECK_NEAR4(fp_pixel(prog(0x34), setup), (std::array<float, 4>{192.f / 255.f, 192.f / 255.f, 128.f / 255.f, 1.f}));
}

TEST_CASE("metal e2e: ARL takes the floor: c[A0.x + 6] with A0.x from -0.5 reads c[5] (NV_vertex_program2, D-174)")
{
    REQUIRE_METAL();
    const auto colour = [](float address) {
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
        VpInst arl; // ARL A0, c[0]
        arl.vec_op = 0x0d;
        arl.src[0] = vp_const();
        arl.constant = 0;
        arl.vec_mask = 0xf;
        VpInst rel; // MOV o[1], c[A0.x + 6]
        rel.vec_op = 1;
        rel.src[0] = vp_const();
        rel.constant = 6;
        rel.constant_relative = true;
        rel.out = 1;
        rel.vec_mask = 0xf;
        VpInst pos; // MOV o[0], v[0] END
        pos.vec_op = 1;
        pos.src[0] = vp_in();
        pos.out = 0;
        pos.vec_mask = 0xf;
        pos.end = true;
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM, assemble_vp({arl, rel, pos}));
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
        b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, 1u << 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT, {f2u(address), f2u(address), f2u(address), f2u(address)});
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 5);
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT, {f2u(0.f), f2u(1.f), f2u(0.f), f2u(1.f),   // c[5] green
                                                       f2u(1.f), f2u(0.f), f2u(0.f), f2u(1.f),   // c[6] red
                                                       f2u(0.f), f2u(0.f), f2u(1.f), f2u(1.f)}); // c[7] blue
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        INFO(failure_text(e));
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        return guest_pixel(e, kSurfaceOffset, 32, 48);
    };
    CHECK(colour(-0.5f) == std::array<u8, 4>{0xff, 0x00, 0xff, 0x00}); // floor: c[5] (RPCS3's truncation: c[6], red)
    CHECK(colour(0.f) == std::array<u8, 4>{0xff, 0xff, 0x00, 0x00});   // c[6]
    CHECK(colour(1.5f) == std::array<u8, 4>{0xff, 0x00, 0x00, 0xff});  // c[7]
}

namespace {

// Texture unit 0: a linear texture of `format` at kTexOffset, w x h with `mips` levels and `pitch`, with the ADDRESS,
// CONTROL0 (the enable bit added), FILTER and CONTROL1 (remap) words given.
void bind_texture_unit0(CommandBuilder& b, u32 format, u32 w, u32 h, u32 mips, u32 pitch, u32 address, u32 control0,
    u32 filter, u32 remap = tex::kRemapIdentity)
{
    b.method(m::NV4097_SET_TEXTURE_OFFSET, {
        kTexOffset,
        (loc::kLocal + 1) | (1u << 3) | (2u << 4) | ((format | tex::kLinearFlag) << 8) | (mips << 16),
        address,
        (1u << 31) | control0,
        remap,
        filter,
        (w << 16) | h,
        0,
    });
    b.method(m::NV4097_SET_TEXTURE_CONTROL3, pitch);
}

constexpr u32 kClampAddress = 3u | (3u << 8) | (3u << 16);
constexpr u32 kNearest = (1u << 16) | (1u << 24);

// TEX R0, f[TEX0], TEX0, with _bx2 or not.
std::vector<FpInst> fp_tex0(bool bx2)
{
    FpInst tex;
    tex.op = 0x17;
    tex.input = 4;
    tex.src[0] = fp_in();
    tex.texture_expand = bx2;
    tex.end = true;
    return {tex};
}

// A 4x4 A8R8G8B8 texture, every texel (A, R, G, B) = (0x80, 0x20, 0x80, 0xC0), sampled through TEX0 with the given
// controls into the float target.
std::array<float, 4> sample_argb(u32 address, u32 filter, u32 remap = tex::kRemapIdentity, bool bx2 = false)
{
    return fp_pixel(fp_tex0(bx2), [&](E2E& e, CommandBuilder& b) {
        for (u32 i = 0; i < 16; i++) e.put_local32(kTexOffset + i * 4, 0x802080C0u);
        bind_texture_unit0(b, tex::A8R8G8B8, 4, 4, 1, 16, kClampAddress | address, 0, kNearest | filter, remap);
    });
}

} // namespace

TEST_CASE("metal e2e: gamma, signed channels and the biased unsigned remap convert texels as RPCS3 does (D-174)")
{
    REQUIRE_METAL();
    const float r = 0x20 / 255.f, g = 0x80 / 255.f, b = 0xC0 / 255.f, a = 0x80 / 255.f;
    CHECK_NEAR4(sample_argb(0, 0), (std::array<float, 4>{r, g, b, a})); // no controls: as stored
    // Gamma on R, G and B (ADDRESS bits 20-22): sRGB to linear; alpha stays.
    CHECK_NEAR4(sample_argb(0x7u << 20, 0), (std::array<float, 4>{srgb_to_linear(r), srgb_to_linear(g), srgb_to_linear(b), a}));
    // Gamma on alpha alone (bit 23).
    CHECK_NEAR4(sample_argb(0x8u << 20, 0), (std::array<float, 4>{r, g, b, srgb_to_linear(a)}));
    // Signed red and blue (FILTER bits 29 and 31): the bytes as two's complement over 127.
    CHECK_NEAR4(sample_argb(0, (0x2u | 0x8u) << 28), (std::array<float, 4>{32.f / 127.f, g, -64.f / 127.f, a}));
    // The biased unsigned remap (ADDRESS bits 12-15 = 1): (byte - 128) / 127 in every channel.
    CHECK_NEAR4(sample_argb(1u << 12, 0), (std::array<float, 4>{-96.f / 127.f, 0.f, 64.f / 127.f, 0.f}));
    // All three: red signed, green and blue gamma, alpha (what is left) biased.
    CHECK_NEAR4(sample_argb((0x7u << 20) | (1u << 12), 0x2u << 28),
        (std::array<float, 4>{32.f / 127.f, srgb_to_linear(g), srgb_to_linear(b), 0.f}));
}

TEST_CASE("metal e2e: _bx2 expands the channels the remap reads on formats that allow it, and replaces gamma (D-174)")
{
    REQUIRE_METAL();
    const auto x2 = [](u32 byte) { return byte / 255.f * 2.f - 1.f; };
    CHECK_NEAR4(sample_argb(0, 0, tex::kRemapIdentity, true), (std::array<float, 4>{x2(0x20), x2(0x80), x2(0xC0), x2(0x80)}));
    // Alpha forced to zero by the remap (control 0): it stays zero (before D-174: -1).
    CHECK_NEAR4(sample_argb(0, 0, 0xA8E4, true), (std::array<float, 4>{x2(0x20), x2(0x80), x2(0xC0), 0.f}));
    // With gamma on: _bx2 wins (RPCS3 _enable_texture_expand erases gamma).
    CHECK_NEAR4(sample_argb(0x7u << 20, 0, tex::kRemapIdentity, true),
        (std::array<float, 4>{x2(0x20), x2(0x80), x2(0xC0), x2(0x80)}));
    // A float format has no biased renormalization: _bx2 does nothing (before D-174: 2x - 1).
    const auto f = fp_pixel(fp_tex0(true), [](E2E& e, CommandBuilder& b) {
        for (u32 i = 0; i < 16; i++) e.put_local_f(kTexOffset + i * 4, 0.25f);
        bind_texture_unit0(b, tex::X32_FLOAT, 4, 4, 1, 16, kClampAddress, 0, kNearest);
    });
    CHECK_NEAR4(f, (std::array<float, 4>{0.25f, 0.25f, 0.25f, 0.25f}));
}

TEST_CASE("metal e2e: a texture's alpha kill discards fragments whose texel has no alpha (D-174)")
{
    REQUIRE_METAL();
    for (const u32 alpha : {0x00u, 0x01u}) {
        INFO(std::format("texel alpha {:#04x}", alpha));
        E2E e;
        e.put_local_bytes(kFpOffset, assemble_fp(fp_tex0(false)));
        write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
        for (u32 i = 0; i < 16; i++) e.put_local32(kTexOffset + i * 4, (alpha << 24) | 0x00ff00u);
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0x11223344);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        bind_texture_unit0(b, tex::A8R8G8B8, 4, 4, 1, 16, kClampAddress, 1u << 2, kNearest); // CONTROL0 bit 2
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        INFO(failure_text(e));
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        const auto expected = alpha ? std::array<u8, 4>{static_cast<u8>(alpha), 0x00, 0xff, 0x00}
                                    : std::array<u8, 4>{0x11, 0x22, 0x33, 0x44}; // discarded: the clear stays
        CHECK(guest_pixel(e, kSurfaceOffset, 32, 48) == expected);
    }
}

TEST_CASE("metal e2e: a texture's LOD bias moves its samples between mipmap levels (D-174)")
{
    REQUIRE_METAL();
    // A 64x64 texture drawn 1:1 (LOD 0), level 0 red and level 1 green, nearest mip filtering, max LOD 12.
    const auto level_colour = [](float bias, bool txl) {
        std::vector<FpInst> prog = fp_tex0(false);
        if (txl) prog[0].op = 0x2F; // TXL: the level is the coordinate's w (1) plus the bias
        return fp_pixel(prog, [&](E2E& e, CommandBuilder& b) {
            for (u32 i = 0; i < 64 * 64; i++) e.put_local32(kTexOffset + i * 4, 0xffff0000u);
            for (u32 y = 0; y < 32; y++)
                for (u32 x = 0; x < 32; x++) e.put_local32(kTexOffset + 64 * 64 * 4 + y * 256 + x * 4, 0xff00ff00u);
            const u32 bias_bits = static_cast<u32>(static_cast<s32>(bias * 256.f)) & 0x1fff; // signed 4.8
            bind_texture_unit0(b, tex::A8R8G8B8, 64, 64, 2, 256, kClampAddress, 0xC00u << 7,
                (3u << 16) | (1u << 24) | bias_bits);
        });
    };
    const std::array<float, 4> red{1.f, 0.f, 0.f, 1.f}, green{0.f, 1.f, 0.f, 1.f};
    CHECK_NEAR4(level_colour(0.f, false), red);
    CHECK_NEAR4(level_colour(1.f, false), green);
    CHECK_NEAR4(level_colour(0.f, true), green);
    CHECK_NEAR4(level_colour(-1.f, true), red);
}

TEST_CASE("metal e2e: NV4097_SET_SHADER_PACKER writes 16-bit exports to 8-bit targets as sRGB (D-174)")
{
    REQUIRE_METAL();
    // Linear values whose sRGB forms are the bytes 0x40, 0x80, 0xC0; alpha 0.25 (0x40), never converted.
    const std::array<float, 4> colour{srgb_to_linear(0x40 / 255.f), srgb_to_linear(0x80 / 255.f), srgb_to_linear(0xC0 / 255.f),
        0.25f};
    const auto draw = [&](bool packer, bool fp16) {
        E2E e;
        FpInst mov = fp_mov_const(0, colour); // MOV H0 or R0, {colour} END
        mov.fp16 = fp16;
        mov.end = true;
        e.put_local_bytes(kFpOffset, assemble_fp({mov}));
        write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, fp16 ? 0 : 0x40);
        b.method(m::NV4097_SET_SHADER_PACKER, packer ? 1 : 0);
        bind_vertex_arrays(b);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        INFO(failure_text(e));
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        return guest_pixel(e, kSurfaceOffset, 32, 48);
    };
    const auto byte = [](float v) { return static_cast<u8>(std::lround(v * 255.f)); };
    const std::array<u8, 4> linear{byte(colour[3]), byte(colour[0]), byte(colour[1]), byte(colour[2])}; // A R G B
    CHECK(draw(true, true) == std::array<u8, 4>{0x40, 0x40, 0x80, 0xC0});
    CHECK(draw(true, false) == linear); // 32-bit exports are never converted (RPCS3)
    CHECK(draw(false, true) == linear);
}

TEST_CASE("metal e2e: the alpha test discards in a pass without colour targets too (RPCS3's ROP, D-174)")
{
    // A depth-only pass (shadow maps of alpha-tested geometry): the triangle writes depth 0.75 over a 1.0 clear where
    // alpha passes GREATER 0.5. Before D-174 the alpha test needed a colour output.
    REQUIRE_METAL();
    constexpr u32 kDepth = 0x180000;
    for (const float alpha : {0.25f, 0.75f}) {
        INFO(std::format("alpha {}", alpha));
        E2E e;
        FpInst mov = fp_mov_const(0, {1.f, 0.f, 0.f, alpha}); // MOV H0, {1, 0, 0, alpha} END
        mov.fp16 = true;
        mov.end = true;
        e.put_local_bytes(kFpOffset, assemble_fp({mov}));
        write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (1u << 5) | (1u << 8)); // Z16
        b.method(m::NV4097_SET_SURFACE_COLOR_TARGET, static_cast<u32>(SurfaceTarget::None));
        b.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
        b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, kDepth);
        b.method(m::NV4097_SET_SURFACE_PITCH_Z, kW * 2);
        b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xffff);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kDepth);
        b.method(m::NV4097_SET_DEPTH_TEST_ENABLE, 1);
        b.method(m::NV4097_SET_DEPTH_FUNC, 0x201); // LESS
        b.method(m::NV4097_SET_DEPTH_MASK, 1);
        b.method(m::NV4097_SET_ALPHA_TEST_ENABLE, 1);
        b.method(m::NV4097_SET_ALPHA_FUNC, 0x204); // GREATER
        b.method(m::NV4097_SET_ALPHA_REF, 0x80);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0);
        bind_vertex_arrays(b);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        INFO(failure_text(e));
        CHECK(e.failures.empty());
        CHECK(e.backend->stats().draws == 1);
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kDepth, kW * kH * 2));
        const u32 depth = ps_ld16(e.mem.host(PS_MEM_RSX_LOCAL_BASE + kDepth + (48 * kW + 32) * 2));
        if (alpha > 0.5f) {
            CHECK(std::abs(static_cast<int>(depth) - 0xBFFF) <= 1); // 0.75
        } else {
            CHECK(depth == 0xFFFF); // discarded
        }
    }
}

TEST_CASE("metal e2e: alpha to coverage keeps a share of the pixels as large as the alpha (RPCS3's emulation, D-174)")
{
    // NV4097_SET_ANTI_ALIASING_CONTROL bit 4 on a one-sample target: RPCS3 discards where alpha is not above a
    // pseudo-random value of the pixel. Counted over a 17x15 box inside the triangle.
    REQUIRE_METAL();
    const auto drawn = [](float alpha) {
        E2E e;
        FpInst mov = fp_mov_const(0, {1.f, 1.f, 1.f, alpha});
        mov.end = true;
        e.put_local_bytes(kFpOffset, assemble_fp({mov}));
        write_triangle(e, {0xffffffff, 0xffffffff, 0xffffffff});
        auto b = e.at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        b.method(m::NV4097_SET_ANTI_ALIASING_CONTROL, 0xffff0000u | (1u << 4));
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        INFO(failure_text(e));
        CHECK(e.failures.empty());
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        u32 n = 0;
        for (u32 y = 48; y < 63; y++)
            for (u32 x = 24; x <= 40; x++) n += guest_pixel(e, kSurfaceOffset, x, y)[1] == 0xff ? 1 : 0;
        return n;
    };
    CHECK(drawn(1.f) == 17 * 15);
    CHECK(drawn(0.f) == 0);
    const u32 half = drawn(0.5f);
    INFO(std::format("{} of {} drawn at alpha 0.5", half, 17 * 15));
    CHECK(half > 17 * 15 / 4);
    CHECK(half < 17 * 15 * 3 / 4);
}

TEST_CASE("metal e2e: unsupported shader opcodes skip the draw loudly")
{
    REQUIRE_METAL();
    E2E e;
    FpInst bad;
    bad.op = 0x26; // POW: RPCS3's decompiler has none either
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
    CHECK(e.failures[0].message.find("POW") != std::string::npos);
}

TEST_CASE("metal: the shader interpreter compiles, its vertex function and every fragment variant, into pipelines (D-940)")
{
    REQUIRE_METAL();
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    const auto library = [&](u32 mrt, bool depth) -> id<MTLLibrary> {
        MTLCompileOptions* o = [MTLCompileOptions new];
        if (@available(macOS 15.0, *)) o.mathMode = MTLMathModeSafe;
        o.preprocessorMacros = @{@"RSX_MRT": @(mrt), @"RSX_DEPTH": @(depth ? 1 : 0)};
        NSError* error = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:shader::interpreter_source().c_str()]
                                                  options:o
                                                    error:&error];
        if (!lib) FAIL(error.localizedDescription.UTF8String);
        return lib;
    };
    id<MTLFunction> vs = [library(1, false) newFunctionWithName:@(shader::kInterpVsEntry)];
    REQUIRE(vs != nil);
    for (u32 mrt = 0; mrt <= 4; mrt++) {
        for (bool depth : {false, true}) {
            INFO("RSX_MRT " << mrt << ", RSX_DEPTH " << depth);
            id<MTLFunction> fs = [library(mrt, depth) newFunctionWithName:@(shader::kInterpFsEntry)];
            REQUIRE(fs != nil);
            // A pipeline of the pair: the vertex function's outputs feed every fragment input.
            MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = vs;
            d.fragmentFunction = fs;
            for (u32 i = 0; i < mrt; i++) d.colorAttachments[i].pixelFormat = i % 2 ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;
            if (depth || mrt == 0) d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            NSError* error = nil;
            id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:d error:&error];
            if (!pso) FAIL(error.localizedDescription.UTF8String);
        }
    }
}

// ---- the shader interpreter against the translation, on random programs (D-940) ----------------------------------
//   PSPOCKET_FUZZ_SEED=<n>   the seed (default 0x5EED)
//   PSPOCKET_FUZZ_ITERS=<n>  rounds (default 1: 64 fragment and 64 vertex programs)

namespace {

u64 fuzz_env(const char* name, u64 fallback)
{
    const char* v = std::getenv(name);
    return v && *v ? std::strtoull(v, nullptr, 0) : fallback;
}

// One backend drawing the test triangle into a 32-bit float target, translated or through the interpreter; each draw
// gets its own program and command offsets.
struct FloatDraws {
    E2E e;
    u32 next = 0;
    explicit FloatDraws(bool interpreted) : e(0, {}, 1800, true, -1, true, interpreted) {}

    std::vector<float> draw(const std::vector<u32>& vp_words, u32 vp_inputs, const std::vector<std::array<float, 4>>& consts,
        const std::vector<u8>& fp_bytes)
    {
        const u32 slot = next++;
        const u32 fp_at = 0x200000 + (slot % 4096) * 0x800; // past the target at kSurfaceOffset
        // Each draw's commands in a slot of their own, the FIFO started at them (it would run through the gap from the
        // last draw's end, and it does not wrap).
        const u32 at = (slot % 512) * 0x800;
        e.rsx->set_fifo_pointers(at, at);
        e.put_local_bytes(fp_at, fp_bytes);
        write_triangle(e, {0xff4080c0, 0x20ff6010, 0x8010ff40});
        auto b = e.at(at);
        setup_surface_as(b, kSurfaceOffset, SurfaceColorFormat::F_W32Z32Y32X32, kW * 16);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
        for (usize i = 0; i < vp_words.size(); i += 32) { // the method takes 32 words at a time
            b.method(m::NV4097_SET_TRANSFORM_PROGRAM,
                std::vector<u32>(vp_words.begin() + static_cast<std::ptrdiff_t>(i),
                    vp_words.begin() + static_cast<std::ptrdiff_t>(std::min(i + 32, vp_words.size()))));
        }
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
        b.method(m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, vp_inputs);
        if (!consts.empty()) {
            b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
            std::vector<u32> words;
            for (const auto& v : consts)
                for (float f : v) words.push_back(f2u(f));
            b.method(m::NV4097_SET_TRANSFORM_CONSTANT, words);
        }
        set_fragment_program(b, fp_at, 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        e.failures.clear();
        REQUIRE(e.backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 16));
        std::vector<float> out(kW * kH * 4);
        for (usize i = 0; i < out.size(); i++) {
            const u32 w = ps_read32(e.mem.base(), PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset + static_cast<u32>(i) * 4);
            std::memcpy(&out[i], &w, 4);
        }
        return out;
    }
};

// The default vertex program: o0 = v0, o1 = v3 (COL0), o7 = v8 (TEX0).
std::vector<u32> plain_vp()
{
    VpInst p0;
    p0.vec_op = 1;
    p0.src[0] = vp_in();
    p0.out = 0;
    p0.vec_mask = 0xf;
    VpInst p1 = p0;
    p1.input = 3;
    p1.out = 1;
    VpInst p2 = p0;
    p2.input = 8;
    p2.out = 7;
    p2.end = true;
    return assemble_vp({p0, p1, p2});
}
constexpr u32 kPlainInputs = (1u << 0) | (1u << 3) | (1u << 8);

// Equal, or both NaN, or within a few units in the last place of the larger. A denormal counts as zero: the GPU may
// flush one (a packed value's bits that read as a denormal: the translation's constant folding flushes them).
bool same_float(float a, float b)
{
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    if (std::fpclassify(a) == FP_SUBNORMAL) a = 0.f;
    if (std::fpclassify(b) == FP_SUBNORMAL) b = 0.f;
    if (a == b) return true;
    return std::abs(a - b) <= 4e-6f * std::max(std::abs(a), std::abs(b));
}

std::array<u8, 4> random_swizzle(std::mt19937& rng)
{
    std::uniform_int_distribution<int> c(0, 3);
    if (rng() % 3 == 0) return {0, 1, 2, 3};
    return {static_cast<u8>(c(rng)), static_cast<u8>(c(rng)), static_cast<u8>(c(rng)), static_cast<u8>(c(rng))};
}

// A random fragment program over R0-R3 and H0-H3, the inputs WPOS, COL0, TEX0, and embedded constants: every
// arithmetic, pack and unpack operation, the modifiers, condition codes, and IFE and LOOP blocks. R0 is its output.
std::vector<FpInst> random_fp_insts(std::mt19937& rng)
{
    static constexpr u32 kOps[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x38, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x1A, 0x1B, 0x1C, 0x1D, 0x1F, 0x22, 0x23, 0x3A, 0x39, 0x07, 0x2E, 0x1E, 0x3C, 0x36, 0x2B, 0x20, 0x21,
        0x3B, 0x27, 0x28, 0x29, 0x2A, 0x13, 0x14, 0x24, 0x25, 0x2C, 0x2D};
    std::uniform_real_distribution<float> value(-4.f, 4.f);
    const auto source = [&]() {
        FpSrc src;
        switch (rng() % 4) {
        case 0: src = fp_in(); break;
        case 1: src = fp_const(); break;
        default: src = rng() % 4 == 0 ? fp_h(rng() % 4) : fp_r(rng() % 4); break;
        }
        src.sw = random_swizzle(rng);
        src.neg = rng() % 5 == 0;
        src.abs = rng() % 6 == 0;
        return src;
    };
    std::vector<FpInst> prog;
    const u32 n = 2 + rng() % 9;
    for (u32 i = 0; i < n; i++) {
        FpInst in;
        in.op = kOps[rng() % std::size(kOps)];
        in.dst = rng() % 4;
        in.fp16 = rng() % 5 == 0;
        in.mask = static_cast<u8>(1 + rng() % 15);
        static constexpr u32 kInputs[] = {0, 1, 4};
        in.input = kInputs[rng() % 3];
        for (FpSrc& s : in.src) s = source();
        if (std::any_of(std::begin(in.src), std::end(in.src), [](const FpSrc& s) { return s.type == 2; }))
            in.constant = std::array<float, 4>{value(rng), value(rng), value(rng), value(rng)};
        in.saturate = rng() % 7 == 0;
        if (rng() % 5 == 0) in.prec = 1 + rng() % 3;
        if (rng() % 6 == 0) in.scale = rng() % 8;
        if (rng() % 5 == 0) {
            in.set_cond = true;
            in.cond_mod_reg = rng() % 2;
            in.no_dest = rng() % 3 == 0;
        }
        if (rng() % 4 == 0) {
            do {
                in.exec_lt = rng() % 2;
                in.exec_eq = rng() % 2;
                in.exec_gt = rng() % 2;
            } while (!in.exec_lt && !in.exec_eq && !in.exec_gt);
            in.cond_reg = rng() % 2;
            in.cond_sw = random_swizzle(rng);
        }
        prog.push_back(in);
    }
    FpInst out;
    out.op = 0x01; // MOV R0, R0..R3 or H0..H3
    out.src[0] = rng() % 4 == 0 ? fp_h(rng() % 4) : fp_r(rng() % 4);
    prog.push_back(out);
    // A block around part of the body: an IFE (with or without an else part) or a LOOP run 1 to 3 times.
    if (prog.size() > 3 && rng() % 2 == 0) {
        const u32 first = rng() % (prog.size() - 2), last = first + 1 + rng() % (prog.size() - 1 - first - 1);
        FpInst block;
        block.op = rng() % 2 ? 0x42 : 0x43; // IFE, LOOP
        do {
            block.exec_lt = rng() % 2;
            block.exec_eq = rng() % 2;
            block.exec_gt = rng() % 2;
        } while (!block.exec_lt && !block.exec_eq && !block.exec_gt);
        block.cond_reg = rng() % 2;
        block.cond_sw = random_swizzle(rng);
        prog.insert(prog.begin() + first, block);
        // Byte offsets: each instruction 16 bytes, and 16 for its constant.
        const auto offset_of = [&](usize index) {
            u32 at = 0;
            for (usize k = 0; k < index; k++) at += prog[k].constant ? 32 : 16;
            return at;
        };
        const u32 end = offset_of(last + 2);
        if (block.op == 0x42) {
            const u32 else_at = rng() % 2 ? offset_of(first + 1 + (last - first) / 2 + 1) : end;
            fp_if(prog[first], else_at, end);
        } else {
            fp_loop(prog[first], 1 + rng() % 3, rng() % 4, 1, end);
        }
    }
    prog.back().end = true;
    return prog;
}

std::vector<u8> random_fp(std::mt19937& rng) { return assemble_fp(random_fp_insts(rng)); }

// A random vertex program: o0 the position as given, then vector and scalar operations (dual issue) over R0-R3, the
// inputs and c[0]-c[7], conditional writes, its result in o7 (TEX0, read unclamped by MOV R0, f[TEX0]).
std::vector<u32> random_vp(std::mt19937& rng)
{
    static constexpr u32 kVec[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22};
    static constexpr u32 kSca[] = {1, 2, 3, 4, 5, 6, 7, 13, 14, 15, 16};
    std::vector<VpInst> prog;
    VpInst pos;
    pos.vec_op = 1;
    pos.src[0] = vp_in();
    pos.out = 0;
    pos.vec_mask = 0xf;
    prog.push_back(pos);
    const auto source = [&]() {
        VpSrc src;
        switch (rng() % 3) {
        case 0: src = vp_in(); break;
        case 1: src = vp_const(); break;
        default: src = vp_temp(rng() % 4); break;
        }
        src.sw = random_swizzle(rng);
        src.neg = rng() % 5 == 0;
        src.abs = rng() % 6 == 0;
        return src;
    };
    const u32 n = 2 + rng() % 9;
    for (u32 i = 0; i < n; i++) {
        VpInst in;
        static constexpr u32 kInputs[] = {0, 3, 8};
        in.input = kInputs[rng() % 3];
        in.constant = rng() % 8;
        for (VpSrc& s : in.src) s = source();
        in.vec_result = false;
        const bool vec = rng() % 5 != 0, sca = !vec || rng() % 3 == 0;
        if (vec) {
            in.vec_op = kVec[rng() % std::size(kVec)];
            in.vec_temp = rng() % 4;
            in.vec_mask = static_cast<u8>(1 + rng() % 15);
        }
        if (sca) {
            in.sca_op = kSca[rng() % std::size(kSca)];
            in.sca_temp = rng() % 4;
            in.sca_mask = static_cast<u8>(1 + rng() % 15);
        }
        in.saturate = rng() % 7 == 0;
        if (rng() % 5 == 0) in.cond_update = true;
        if (rng() % 4 == 0) {
            in.cond_test = true;
            in.cond = 1 + rng() % 6;
            in.cond_sw = random_swizzle(rng);
        }
        prog.push_back(in);
    }
    VpInst out;
    out.vec_op = 1;
    out.src[0] = vp_temp(rng() % 4);
    out.out = 7;
    out.vec_mask = 0xf;
    out.end = true;
    prog.push_back(out);
    return assemble_vp(prog);
}

} // namespace

TEST_CASE("metal: the shader interpreter draws random programs as their translation does (D-940)")
{
    REQUIRE_METAL();
    const u64 seed = fuzz_env("PSPOCKET_FUZZ_SEED", 0x5EED), rounds = std::max<u64>(1, fuzz_env("PSPOCKET_FUZZ_ITERS", 1));
    std::mt19937 rng(static_cast<u32>(seed));
    FloatDraws translated(false), interpreted(true);
    std::uniform_real_distribution<float> value(-4.f, 4.f);
    // MOV R0, f[TEX0]: the vertex programs' results.
    FpInst mov;
    mov.op = 0x01;
    mov.input = 4;
    mov.src[0] = fp_in();
    mov.end = true;
    const std::vector<u8> tex0 = assemble_fp({mov});
    const auto compare = [](const std::vector<float>& a, const std::vector<float>& b, u64 round, u32 index, bool vertex,
                             const std::string& program) {
        usize differ = 0, first = a.size();
        for (usize i = 0; i < a.size(); i++) {
            if (same_float(a[i], b[i])) continue;
            if (!differ++) first = i;
        }
        INFO(program);
        const usize px = first / 4 * 4;
        const auto pixel = [&](const std::vector<float>& v) {
            return px + 3 < v.size() ? std::format("({}, {}, {}, {})", v[px], v[px + 1], v[px + 2], v[px + 3]) : std::string();
        };
        INFO((vertex ? "vertex" : "fragment") << " program " << index << " of round " << round << ": " << differ
             << " floats differ, the first at " << first << ": pixel " << px / 4 << " translated " << pixel(a)
             << ", interpreted " << pixel(b));
        CHECK(differ == 0);
    };
    for (u64 round = 0; round < rounds; round++) {
        for (u32 i = 0; i < 64; i++) {
            const std::vector<u8> fp = random_fp(rng);
            const auto decoded = shader::decode_fragment_program(fp.data(), static_cast<u32>(fp.size()));
            std::string text = decoded ? shader::disassemble(*decoded) : std::string("(no program)");
            if (decoded && std::getenv("PSPOCKET_TEST_DUMP")) { // the translation too
                shader::FpOptions fo;
                fo.shader_control = 0x40;
                text += "\n" + shader::translate_fragment_program(*decoded, fo).shader.source;
            }
            if (const char* only = std::getenv("PSPOCKET_TEST_ONLY")) {
                if (std::strtoul(only, nullptr, 0) != i) continue;
                FloatDraws fresh(false), fresh_i(true);
                compare(fresh.draw(plain_vp(), kPlainInputs, {}, fp), fresh_i.draw(plain_vp(), kPlainInputs, {}, fp), round, i, false, text);
                // Each instruction a NOP in turn (its constant kept, so offsets stay): does the difference stay?
                std::mt19937 again(static_cast<u32>(seed));
                std::vector<FpInst> insts;
                for (u32 k = 0; k <= i; k++) insts = random_fp_insts(again);
                REQUIRE(assemble_fp(insts) == fp);
                for (usize k = 0; k + 1 < insts.size(); k++) {
                    if (insts[k].flow_src1) continue;
                    std::vector<FpInst> cut = insts;
                    cut[k].op = 0;
                    cut[k].set_cond = false;
                    const std::vector<u8> bytes = assemble_fp(cut);
                    const auto a = fresh.draw(plain_vp(), kPlainInputs, {}, bytes);
                    const auto b = fresh_i.draw(plain_vp(), kPlainInputs, {}, bytes);
                    usize differ = 0;
                    for (usize q = 0; q < a.size(); q++) differ += same_float(a[q], b[q]) ? 0 : 1;
                    std::printf("without instruction %zu: %zu floats differ\n", k, differ);
                }
                continue;
            }
            compare(translated.draw(plain_vp(), kPlainInputs, {}, fp), interpreted.draw(plain_vp(), kPlainInputs, {}, fp),
                round, i, false, text);
        }
        for (u32 i = 0; i < 64; i++) {
            const std::vector<u32> vp = random_vp(rng);
            std::vector<std::array<float, 4>> consts(8);
            for (auto& c : consts) c = {value(rng), value(rng), value(rng), value(rng)};
            const auto decoded = shader::decode_vertex_program(vp.data(), static_cast<u32>(vp.size() / 4), 0);
            compare(translated.draw(vp, kPlainInputs, consts, tex0), interpreted.draw(vp, kPlainInputs, consts, tex0), round,
                i, true, decoded ? shader::disassemble(*decoded) : std::string("(no program)"));
        }
    }
    CHECK(translated.e.backend->stats().draws_interpreted == 0);
    CHECK(interpreted.e.backend->stats().draws_interpreted == interpreted.e.backend->stats().draws);
}

TEST_CASE("metal: pspocket's packs are Metal's pack_float_to_* bit for bit (D-942)")
{
    REQUIRE_METAL();
    // Metal's intrinsics in a kernel with nothing else written next to them (where the compiler gets them right),
    // against the hand-written ones the translation and the interpreter use: every code's value and half-step, and
    // random values in and past the range.
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    const std::string src = std::string("#include <metal_stdlib>\nusing namespace metal;\n") + shader::msl_pack_helpers() + R"(
kernel void k(device const float4* in [[buffer(0)]], device atomic_uint* bad [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    const float4 v = in[i];
    if (rsx_pack_unorm4x8(v) != pack_float_to_unorm4x8(v)) atomic_fetch_add_explicit(&bad[0], 1u, memory_order_relaxed);
    if (rsx_pack_snorm4x8(v) != pack_float_to_snorm4x8(v)) atomic_fetch_add_explicit(&bad[1], 1u, memory_order_relaxed);
    if (rsx_pack_unorm2x16(v.xy) != pack_float_to_unorm2x16(v.xy)) atomic_fetch_add_explicit(&bad[2], 1u, memory_order_relaxed);
}
)";
    MTLCompileOptions* o = [MTLCompileOptions new];
    if (@available(macOS 15.0, *)) o.mathMode = MTLMathModeSafe;
    NSError* error = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:@(src.c_str()) options:o error:&error];
    if (!lib) FAIL(error.localizedDescription.UTF8String);
    id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&error];
    REQUIRE(pso != nil);
    std::vector<std::array<float, 4>> in;
    for (u32 c = 0; c <= 65535; c++) {
        const float u = static_cast<float>(c) / 65535.f, h = (static_cast<float>(c) + 0.5f) / 65535.f;
        const float b = static_cast<float>(c & 255) / 255.f, bh = (static_cast<float>(c & 255) + 0.5f) / 255.f;
        in.push_back({u, h, b, bh});
        in.push_back({-u, -h, -b, -bh});
        in.push_back({(static_cast<float>(c & 127) + 0.5f) / 127.f, -(static_cast<float>(c & 127) + 0.5f) / 127.f,
            static_cast<float>(c & 127) / 127.f, bh * 2.f - 1.f});
    }
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> any(-2.f, 2.f);
    for (u32 i = 0; i < 100000; i++) in.push_back({any(rng), any(rng), any(rng), any(rng)});
    in.push_back({std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 0.f, -0.f});
    id<MTLBuffer> ib = [device newBufferWithBytes:in.data() length:in.size() * 16 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bad = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
    std::memset(bad.contents, 0, 16);
    id<MTLCommandQueue> q = [device newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:ib offset:0 atIndex:0];
    [enc setBuffer:bad offset:0 atIndex:1];
    [enc dispatchThreads:MTLSizeMake(in.size(), 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const u32* b = static_cast<const u32*>(bad.contents);
    CHECK(b[0] == 0);
    CHECK(b[1] == 0);
    CHECK(b[2] == 0);
}

TEST_CASE("metal e2e: a pack next to another write to its register's components draws as written (D-942)")
{
    REQUIRE_METAL();
    // PK16R R0.zw, H3; DP2AR R0.xw, f[TEX0], c.xyyz, c.zzyw; MOVR R0, R0: with Metal's pack_float_to_unorm2x16 the
    // compiled program lost the DP2A's w (and the pack's z), translated or interpreted.
    FpInst pk;
    pk.op = 0x29;
    pk.mask = mask("zw");
    pk.src[0] = fp_h(3);
    FpInst dp;
    dp.op = 0x2E;
    dp.mask = mask("xw");
    dp.input = 4;
    dp.src[0] = fp_in();
    dp.src[1] = fp_const("xyyz");
    dp.src[2] = fp_const("zzyw");
    dp.constant = std::array<float, 4>{-2.7386088f, 2.415514f, 2.1589785f, -0.497972f};
    FpInst mov;
    mov.op = 0x01;
    mov.src[0] = fp_r(0);
    mov.end = true;
    const std::vector<u8> fp = assemble_fp({pk, dp, mov});
    for (bool interpreted : {false, true}) {
        INFO("interpreted " << interpreted);
        FloatDraws d(interpreted);
        const auto px = d.draw(plain_vp(), kPlainInputs, {}, fp);
        const usize at = (32 + 40 * kW) * 4; // inside the triangle
        CHECK(px[at] != 0.f);
        CHECK(px[at + 3] == px[at]); // DP2A's w
        CHECK(px[at + 2] == 0.f);    // the pack of H3's (0, 0)
    }
}

// ---- fewer render passes (D-945) -----------------------------------------------------------------------------------

namespace {

// An E2E whose backend merges passes (D-945) or, with `merging` false, runs every clear where it is, as before.
std::unique_ptr<E2E> e2e_merging(bool merging)
{
    if (!merging) setenv("PSPOCKET_RSX_NO_PASS_MERGING", "1", 1);
    auto e = std::make_unique<E2E>();
    unsetenv("PSPOCKET_RSX_NO_PASS_MERGING");
    return e;
}

// R0 = f[COL0] for target A, R2 = green for target B (32-bit exports).
std::vector<u8> fp_two_targets()
{
    FpInst green;
    green.op = 0x01;
    green.dst = 2;
    green.src[0] = fp_const();
    green.constant = std::array<float, 4>{0.f, 1.f, 0.f, 1.f};
    FpInst col;
    col.op = 0x01;
    col.dst = 0;
    col.input = 1;
    col.src[0] = fp_in();
    col.end = true;
    return assemble_fp({green, col});
}

// R0 = green.
std::vector<u8> fp_green()
{
    FpInst green;
    green.op = 0x01;
    green.src[0] = fp_const();
    green.constant = std::array<float, 4>{0.f, 1.f, 0.f, 1.f};
    green.end = true;
    return assemble_fp({green});
}

// A Z16 depth buffer at `offset`, kW x kH (the surface format's depth bits set to Z16 too).
void set_z16(CommandBuilder& b, u32 offset)
{
    b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (1u << 5) | (1u << 8));
    b.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
    b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, offset);
    b.method(m::NV4097_SET_SURFACE_PITCH_Z, kW * 2);
}

// Colour targets A and B (A8R8G8B8, kW x kH), both written, and a Z16 depth buffer when `depth` is given.
void setup_targets_ab(CommandBuilder& b, u32 a, u32 b_offset, u32 depth = 0)
{
    setup_surface(b, a);
    if (depth) set_z16(b, depth);
    b.method(m::NV4097_SET_CONTEXT_DMA_COLOR_B, loc::kDmaFrameBuffer);
    b.method(m::NV4097_SET_SURFACE_COLOR_BOFFSET, b_offset);
    b.method(m::NV4097_SET_SURFACE_PITCH_B, kW * 4);
    b.method(m::NV4097_SET_SURFACE_COLOR_TARGET, static_cast<u32>(SurfaceTarget::AB));
    b.method(m::NV4097_SET_COLOR_MASK_MRT, 0xfff0); // targets B to D written
}

void depth_test_less(CommandBuilder& b)
{
    b.method(m::NV4097_SET_DEPTH_TEST_ENABLE, 1);
    b.method(m::NV4097_SET_DEPTH_FUNC, static_cast<u32>(CompareFunc::Less));
    b.method(m::NV4097_SET_DEPTH_MASK, 1);
}

u16 guest_depth16(E2E& e, u32 offset, u32 x, u32 y)
{
    return ps_read16(e.mem.base(), PS_MEM_RSX_LOCAL_BASE + offset + y * kW * 2 + x * 2);
}

constexpr u32 kDepthZ = 0x180000;  // Z16, kW x kH
constexpr u32 kSurfaceC = 0x1a0000; // a third colour target

using Px = std::array<u8, 4>; // guest A8R8G8B8 bytes: a r g b
constexpr Px kRed{0xff, 0xff, 0x00, 0x00}, kGreen{0xff, 0x00, 0xff, 0x00}, kBlue{0xff, 0x00, 0x00, 0xff};
constexpr Px kBlack{0xff, 0x00, 0x00, 0x00}, kNothing{0x00, 0x00, 0x00, 0x00};

} // namespace

TEST_CASE("metal e2e: clears of whole targets are the next pass's load actions, not passes of their own (D-945)")
{
    // GTA V clears the first of its G-buffer's four targets alone, then its depth buffer alone, then draws to all four
    // and the depth buffer: two passes that only stored what the third loaded again (7 MB a frame, D-273).
    REQUIRE_METAL();
    struct Result {
        Px a_in, a_out, b_in, b_out;
        u16 z_in = 0, z_out = 0;
        u64 passes = 0, folded = 0, clear_only = 0, loaded = 0;
    };
    const auto run = [](bool merging, u32 zclear) {
        auto e = e2e_merging(merging);
        e->put_local_bytes(kFpOffset, fp_two_targets());
        write_triangle(*e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red, window z 0.75
        auto b = e->at(0);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff0000ffu); // blue
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);      // target A alone
        set_z16(b, kDepthZ);
        b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, zclear);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kDepth); // the depth buffer alone
        setup_targets_ab(b, kSurfaceOffset, kSurfaceB, kDepthZ);
        depth_test_less(b);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        draw_triangle(b);
        REQUIRE(e->submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e->failures) INFO(f.message);
        CHECK(e->failures.empty());
        Result r;
        const auto& st = e->backend->stats();
        r.passes = st.render_passes;
        r.folded = st.clears_folded;
        r.clear_only = st.clear_only_passes;
        r.loaded = st.attachment_load_bytes;
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH * 4));
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kDepthZ, kW * kH * 2));
        r.a_in = guest_pixel(*e, kSurfaceOffset, 32, 48), r.a_out = guest_pixel(*e, kSurfaceOffset, 2, 2);
        r.b_in = guest_pixel(*e, kSurfaceB, 32, 48), r.b_out = guest_pixel(*e, kSurfaceB, 2, 2);
        r.z_in = guest_depth16(*e, kDepthZ, 32, 48), r.z_out = guest_depth16(*e, kDepthZ, 2, 2);
        return r;
    };
    for (const u32 zclear : {0xffffu, 0x4000u}) { // 1.0: the triangle passes the depth test; 0.25: it does not
        INFO(std::format("depth cleared to {:#x}", zclear));
        const Result on = run(true, zclear), off = run(false, zclear);
        const bool drawn = zclear == 0xffff;
        CHECK(on.a_in == (drawn ? kRed : kBlue));
        CHECK(on.a_out == kBlue);
        CHECK(on.b_in == (drawn ? kGreen : kNothing)); // B was never written: it starts cleared
        CHECK(on.b_out == kNothing);
        // Metal makes a Z16 clear of 0x4000 (0.25000381) 0x3fff: within one step.
        CHECK(std::abs(static_cast<int>(on.z_in) - static_cast<int>(drawn ? 0xbfffu : zclear)) <= 1);
        CHECK(std::abs(static_cast<int>(on.z_out) - static_cast<int>(zclear)) <= 1);
        // The same pixels as clears in passes of their own.
        CHECK(on.a_in == off.a_in);
        CHECK(on.a_out == off.a_out);
        CHECK(on.b_in == off.b_in);
        CHECK(on.b_out == off.b_out);
        CHECK(on.z_in == off.z_in);
        CHECK(on.z_out == off.z_out);
        CHECK(on.passes == 1);
        CHECK(on.folded == 2);
        CHECK(on.clear_only == 0);
        CHECK(on.loaded == 0); // nothing loaded: both clears are the pass's load actions, B starts cleared
        CHECK(off.passes == 3);
        CHECK(off.clear_only == 2);
        CHECK(off.loaded == kW * kH * 4 + kW * kH * 2); // the pass loaded what the two clears stored
    }
}

TEST_CASE("metal e2e: a clear of a target the pass does not draw to waits, and the pass goes on (D-945)")
{
    REQUIRE_METAL();
    struct Result {
        Px a_in, a_out, b_left, b_tri_left, b_tri_right, b_right;
        u64 passes = 0, waited = 0;
    };
    const auto run = [](bool merging, bool part) {
        auto e = e2e_merging(merging);
        e->put_local_bytes(kFpOffset, fp_color(false));
        e->put_local_bytes(kFp2Offset, fp_green());
        write_triangle(*e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red
        auto b = e->at(0);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        setup_surface(b, kSurfaceB);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000u);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        draw_triangle(b); // B: a red triangle on black
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        draw_triangle(b); // A: the same
        setup_surface(b, kSurfaceB);
        if (part) b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {(32u << 16) | 0, (kH << 16) | 0}); // B's left half
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff0000ffu);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor); // B blue: A's pass goes on
        setup_surface(b, kSurfaceOffset);
        set_fragment_program(b, kFp2Offset, 0x40);
        draw_triangle(b); // A again: green over the red
        REQUIRE(e->submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e->failures) INFO(f.message);
        CHECK(e->failures.empty());
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH * 4));
        Result r;
        r.passes = e->backend->stats().render_passes;
        r.waited = e->backend->stats().clears_waited;
        r.a_in = guest_pixel(*e, kSurfaceOffset, 32, 48), r.a_out = guest_pixel(*e, kSurfaceOffset, 2, 2);
        r.b_left = guest_pixel(*e, kSurfaceB, 2, 40), r.b_tri_left = guest_pixel(*e, kSurfaceB, 20, 48);
        r.b_tri_right = guest_pixel(*e, kSurfaceB, 40, 48), r.b_right = guest_pixel(*e, kSurfaceB, 62, 5);
        return r;
    };
    for (const bool part : {false, true}) {
        INFO((part ? "the left half of B cleared" : "all of B cleared"));
        const Result on = run(true, part), off = run(false, part);
        CHECK(on.a_in == kGreen);
        CHECK(on.a_out == kBlack);
        CHECK(on.b_left == kBlue);
        CHECK(on.b_tri_left == kBlue);
        CHECK(on.b_tri_right == (part ? kRed : kBlue));
        CHECK(on.b_right == (part ? kBlack : kBlue));
        for (const auto& [x, y] : {std::pair{on.a_in, off.a_in}, {on.a_out, off.a_out}, {on.b_left, off.b_left},
                 {on.b_tri_left, off.b_tri_left}, {on.b_tri_right, off.b_tri_right}, {on.b_right, off.b_right}})
            CHECK(x == y);
        // B, A (both of its draws), then B's clear in a pass of its own; before: A's pass ended at the clear.
        CHECK(on.passes == 3);
        CHECK(on.waited == 1);
        CHECK(off.passes == 4);
    }
}

TEST_CASE("metal e2e: a draw that samples a target whose clear waits sees the clear (D-945)")
{
    REQUIRE_METAL();
    for (const bool merging : {true, false}) {
        INFO((merging ? "merging" : "not merging"));
        auto e = e2e_merging(merging);
        e->put_local_bytes(kFpOffset, fp_color(false));
        e->put_local_bytes(kFp2Offset, fp_texture());
        write_triangle(*e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red
        auto b = e->at(0);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        setup_surface(b, kSurfaceB);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000u);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        draw_triangle(b); // B: red on black
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        draw_triangle(b); // A's pass
        setup_surface(b, kSurfaceB);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff0000ffu);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor); // B blue, waiting
        setup_surface(b, kSurfaceOffset);
        set_fragment_program(b, kFp2Offset, 0x40);
        bind_linear_texture(b, kSurfaceB, tex::A8R8G8B8, kW * 4);
        draw_triangle(b); // A samples B where B's triangle was: blue, not red
        REQUIRE(e->submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e->failures) INFO(f.message);
        CHECK(e->failures.empty());
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        CHECK(guest_pixel(*e, kSurfaceOffset, 32, 48) == kBlue);
        CHECK(guest_pixel(*e, kSurfaceOffset, 2, 2) == kBlack);
    }
}

TEST_CASE("metal e2e: a pass does not store a depth buffer the next pass clears whole, unless it is read in between (D-945)")
{
    REQUIRE_METAL();
    struct Result {
        Px a_in, b_in, c_in;
        u16 z_in = 0, z_out = 0;
        u64 skipped = 0, stored = 0;
    };
    const auto run = [](bool merging, bool read_between) {
        auto e = e2e_merging(merging);
        e->put_local_bytes(kFpOffset, fp_color(false));
        e->put_local_bytes(kFp2Offset, fp_texture());
        write_triangle(*e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red, window z 0.75
        auto b = e->at(0);
        upload_vertex_program(b);
        bind_vertex_arrays(b);
        set_fragment_program(b, kFpOffset, 0x40);
        // Pass 1: A with the depth buffer.
        setup_surface(b, kSurfaceOffset);
        set_z16(b, kDepthZ);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000u);
        b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xffff);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor | clear::kDepth);
        depth_test_less(b);
        draw_triangle(b);
        if (read_between) {
            // C samples the depth buffer pass 1 left (the depth sampled directly, D-106): pass 1 must store it.
            setup_surface(b, kSurfaceC);
            b.method(m::NV4097_SET_DEPTH_TEST_ENABLE, 0);
            set_fragment_program(b, kFp2Offset, 0x40);
            bind_linear_texture(b, kDepthZ, tex::DEPTH16, kW * 2);
            draw_triangle(b);
            set_fragment_program(b, kFpOffset, 0x40);
            depth_test_less(b);
        }
        // Pass 2: B with the depth buffer, cleared whole first.
        setup_surface(b, kSurfaceB);
        set_z16(b, kDepthZ);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor | clear::kDepth);
        draw_triangle(b);
        REQUIRE(e->submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e->failures) INFO(f.message);
        CHECK(e->failures.empty());
        Result r;
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH * 4));
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kDepthZ, kW * kH * 2));
        if (read_between) REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceC, kW * kH * 4));
        r.skipped = e->backend->stats().attachment_store_skipped_bytes;
        r.stored = e->backend->stats().attachment_store_bytes;
        r.a_in = guest_pixel(*e, kSurfaceOffset, 32, 48);
        r.b_in = guest_pixel(*e, kSurfaceB, 32, 48);
        if (read_between) r.c_in = guest_pixel(*e, kSurfaceC, 32, 48);
        r.z_in = guest_depth16(*e, kDepthZ, 32, 48), r.z_out = guest_depth16(*e, kDepthZ, 2, 2);
        return r;
    };
    for (const bool read_between : {false, true}) {
        INFO((read_between ? "read between the passes" : "nothing between the passes"));
        const Result on = run(true, read_between), off = run(false, read_between);
        CHECK(on.a_in == kRed);
        CHECK(on.b_in == kRed); // the depth test passes against the cleared buffer
        CHECK(on.z_in == 0xbfff);
        CHECK(on.z_out == 0xffff);
        CHECK(on.a_in == off.a_in);
        CHECK(on.b_in == off.b_in);
        CHECK(on.z_in == off.z_in);
        CHECK(on.z_out == off.z_out);
        if (read_between) {
            CHECK(on.c_in == off.c_in); // what pass 1 stored
            CHECK(on.c_in[1] == 0xbf);  // its 0.75, not the 1.0 pass 2 clears to
            CHECK(on.skipped == 0);
        } else {
            CHECK(on.skipped == kW * kH * 2); // pass 1's depth
        }
        CHECK(off.skipped == 0);
    }
}

TEST_CASE("metal e2e: a clear of the first targets of a pass with more leaves the others as they are (D-945)")
{
    // The clear goes to the targets the surface registers name (RPCS3 VKGSRender.cpp clear_surface): in a pass over A
    // and B, a clear made with A alone named cleared B too, by its quad or its load action.
    REQUIRE_METAL();
    for (const bool merging : {true, false}) {
        for (const bool drawn : {true, false}) { // the pass over A and B has drawn (a quad) or not (a load action)
            INFO(std::format("{}, {}", merging ? "merging" : "not merging", drawn ? "after a draw" : "before any draw"));
            auto e = e2e_merging(merging);
            e->put_local_bytes(kFpOffset, fp_two_targets());
            write_triangle(*e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red into A, green into B
            auto b = e->at(0);
            setup_targets_ab(b, kSurfaceOffset, kSurfaceB);
            b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000u);
            b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
            if (drawn) {
                upload_vertex_program(b);
                set_fragment_program(b, kFpOffset, 0x40);
                bind_vertex_arrays(b);
                draw_triangle(b);
            }
            b.method(m::NV4097_SET_SURFACE_COLOR_TARGET, static_cast<u32>(SurfaceTarget::A));
            b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff0000ffu);
            b.method(m::NV4097_CLEAR_SURFACE, clear::kColor); // A alone: blue
            REQUIRE(e->submit(b) == Rsx::RunStatus::Idle);
            for (const auto& f : e->failures) INFO(f.message);
            CHECK(e->failures.empty());
            REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
            REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH * 4));
            CHECK(guest_pixel(*e, kSurfaceOffset, 32, 48) == kBlue);
            CHECK(guest_pixel(*e, kSurfaceOffset, 2, 2) == kBlue);
            CHECK(guest_pixel(*e, kSurfaceB, 32, 48) == (drawn ? kGreen : kBlack));
            CHECK(guest_pixel(*e, kSurfaceB, 2, 2) == kBlack);
        }
    }
}

TEST_CASE("metal e2e: a depth buffer cleared alone after a pass over a smaller target with it is cleared whole (D-951)")
{
    // GTA V's fight (run 327): a 320x128 target drawn with the 1280x720 depth buffer, a clear of another target that
    // waits while that pass goes on (D-945), then a clear of the depth buffer alone under the full surface clip. Done
    // in the open pass, the clear reached the pass's corner only, and the scene drawn next failed the depth test
    // everywhere else: the screen black but its top-left corner.
    REQUIRE_METAL();
    struct Result {
        Px in, top;
        u16 z_far = 0, z_corner = 0;
    };
    const auto run = [](bool merging) {
        auto e = e2e_merging(merging);
        e->put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(*e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red, window z 0.75
        auto b = e->at(0);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        // A black, the depth buffer at 0.25 all over: the triangle fails against it.
        setup_surface(b, kSurfaceOffset);
        set_z16(b, kDepthZ);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000u);
        b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0x4000u);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor | clear::kDepth);
        // A 16x16 target drawn with it, depth-tested: a pass over the depth buffer's top-left corner.
        setup_surface_sized(b, kSurfaceC, 16, 16, kW * 4);
        set_z16(b, kDepthZ);
        depth_test_less(b);
        draw_triangle(b);
        // Another target cleared whole: it waits, and the pass goes on (D-945).
        setup_surface(b, kSurfaceB);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        // The depth buffer alone, all of it, to 1.0.
        setup_surface(b, kSurfaceOffset);
        set_z16(b, kDepthZ);
        b.method(m::NV4097_SET_SURFACE_COLOR_TARGET, static_cast<u32>(SurfaceTarget::None));
        b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xffffu);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kDepth);
        // The scene: the triangle into A against it, drawn everywhere it covers.
        b.method(m::NV4097_SET_SURFACE_COLOR_TARGET, static_cast<u32>(SurfaceTarget::A));
        draw_triangle(b);
        REQUIRE(e->submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e->failures) INFO(f.message);
        CHECK(e->failures.empty());
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kDepthZ, kW * kH * 2));
        Result r;
        r.in = guest_pixel(*e, kSurfaceOffset, 32, 48);
        r.top = guest_pixel(*e, kSurfaceOffset, 31, 15); // the triangle's tip, just right of the 16x16 corner
        r.z_far = guest_depth16(*e, kDepthZ, 60, 4); // outside the triangle and the corner
        r.z_corner = guest_depth16(*e, kDepthZ, 2, 2);
        return r;
    };
    for (const bool merging : {true, false}) {
        INFO((merging ? "merging" : "not merging"));
        const Result r = run(merging);
        CHECK(r.in == kRed); // fails before the fix with merging: the depth there still 0.25
        CHECK(r.top == kRed);
        CHECK(r.z_far == 0xffff);
        CHECK(r.z_corner == 0xffff);
    }
}

TEST_CASE("metal e2e: a clear that waits runs before an NV0039 copy, an NV308A write or a flip reads its target (D-945)")
{
    REQUIRE_METAL();
    for (const bool merging : {true, false}) {
        INFO((merging ? "merging" : "not merging"));
        auto e = e2e_merging(merging);
        e->put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(*e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red
        e->rsx->set_display_buffer(0, {kSurfaceC, kW * 4, kW, kH});
        auto b = e->at(0);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000u);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        draw_triangle(b); // A's pass draws
        setup_surface(b, kSurfaceB);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff0000ffu);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor); // B blue, waiting
        setup_surface(b, kSurfaceC);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff00ff00u);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor); // C green, waiting: the display buffer
        setup_surface(b, kSurfaceOffset);
        draw_triangle(b); // A's pass goes on
        // B to main memory through NV0039 (on the GPU, D-107): blue.
        b.method(m::NV0039_SET_CONTEXT_DMA_BUFFER_IN, {loc::kDmaFrameBuffer, loc::kDmaHostBuffer});
        b.method(m::NV0039_OFFSET_IN, {kSurfaceB, 0x100000, kW * 4, kW * 4, kW * 4, kH, 0x101});
        b.method(m::NV0039_BUFFER_NOTIFY, 0);
        // A pixel of B written by NV308A: B goes to memory first, with its clear (D-110).
        b.method(m::NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN, loc::kDmaFrameBuffer);
        b.method(m::NV3062_SET_COLOR_FORMAT, {10, ((kW * 4) << 16) | (kW * 4), 0, kSurfaceB});
        b.method(m::NV308A_POINT, {0, 1, 1});
        b.method(m::NV308A_COLOR, 0xffff0000u); // red at (0, 0)
        b.method(m::GCM_FLIP_COMMAND, 0);       // shows C: green
        REQUIRE(e->submit(b) == Rsx::RunStatus::Idle);
        e->backend->sync();
        for (const auto& f : e->failures) INFO(f.message);
        CHECK(e->failures.empty());
        const u8* main = e->mem.host(E2E::kReadbackEa);
        CHECK(Px{main[(5 * kW + 5) * 4], main[(5 * kW + 5) * 4 + 1], main[(5 * kW + 5) * 4 + 2],
                  main[(5 * kW + 5) * 4 + 3]} == kBlue);
        // B went to memory before the NV308A write (its clear in it), and memory is newer now: nothing to write back.
        CHECK_FALSE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH * 4));
        CHECK(guest_pixel(*e, kSurfaceB, 0, 0) == kRed);
        CHECK(guest_pixel(*e, kSurfaceB, 32, 48) == kBlue);
        REQUIRE(e->present_pixels.size() == 1);
        const auto& px = e->present_pixels[0];
        CHECK(std::array<u8, 4>{px[(64 * 128 + 64) * 4], px[(64 * 128 + 64) * 4 + 1], px[(64 * 128 + 64) * 4 + 2],
                  px[(64 * 128 + 64) * 4 + 3]} == std::array<u8, 4>{0x00, 0xff, 0x00, 0xff}); // green, BGRA
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        CHECK(guest_pixel(*e, kSurfaceOffset, 32, 48) == kRed);
    }
}

TEST_CASE("metal e2e: clears of part of a target that wait are the first draws of the next pass that has it (D-945)")
{
    // GTA V clears part of a target and its depth buffer, then part of the stencil, with another target's pass still
    // drawing, then draws to them: the clears wait, and the pass over them does them first, in order. A clear of all of
    // a target made after one of part of it makes that one moot; one made before it is the pass's load action.
    REQUIRE_METAL();
    constexpr u32 kZ24 = 0x1c0000; // Z24S8, kW x kH
    struct Result {
        Px a_region, a_out, b_region, b_out;
        u32 z_region = 0, z_out = 0;
        u64 passes = 0, folded = 0;
    };
    // order 0: part of B and its depth, then part of the stencil; 1: all of B, then part of it; 2: part, then all.
    const auto run = [](bool merging, int order) {
        auto e = e2e_merging(merging);
        e->put_local_bytes(kFpOffset, fp_color(false));
        e->put_local_bytes(kFp3Offset, fp_two_targets());
        write_triangle(*e, {0xff0000ff, 0xff0000ff, 0xff0000ff}); // red into A, green into B (MRT)
        auto b = e->at(0);
        upload_vertex_program(b);
        bind_vertex_arrays(b);
        set_fragment_program(b, kFpOffset, 0x40);
        b.method(m::NV4097_SET_STENCIL_MASK, 0xff);
        // A: red on black, in a pass that ends; C: a pass that is drawing when the clears come.
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff000000u);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        draw_triangle(b);
        setup_surface(b, kSurfaceC);
        draw_triangle(b);
        // The clears, of B (target B of the pass after) and the depth buffer.
        const auto region = [&] { b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {(24u << 16) | 4u, (20u << 16) | 30u}); };
        setup_targets_ab(b, kSurfaceOffset, kSurfaceB);
        b.method(m::NV4097_SET_SURFACE_COLOR_TARGET, static_cast<u32>(SurfaceTarget::B));
        b.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
        b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, kZ24);
        b.method(m::NV4097_SET_SURFACE_PITCH_Z, kW * 4);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff0000ffu);   // blue
        b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0x40000033u); // depth 0.25, stencil 0x33
        if (order == 0) {
            region();
            b.method(m::NV4097_CLEAR_SURFACE, clear::kColor | clear::kDepthStencil);
            b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0x40000055u);
            b.method(m::NV4097_CLEAR_SURFACE, clear::kStencil); // the stencil there again: 0x55
        } else if (order == 1) {
            b.method(m::NV4097_CLEAR_SURFACE, clear::kColor | clear::kDepthStencil); // all of B and the depth buffer
            region();
            b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff00ff00u);
            b.method(m::NV4097_CLEAR_SURFACE, clear::kColor); // then green there
        } else {
            region();
            b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff00ff00u);
            b.method(m::NV4097_CLEAR_SURFACE, clear::kColor); // green there
            b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {kW << 16, kH << 16});
            b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff0000ffu);
            b.method(m::NV4097_CLEAR_SURFACE, clear::kColor | clear::kDepthStencil); // then all of it blue
        }
        // The pass over A and B with the depth buffer, drawing nothing new where the region is (depth test LESS against
        // 0.25 there fails; 0.75 elsewhere against what is there).
        setup_targets_ab(b, kSurfaceOffset, kSurfaceB);
        b.method(m::NV4097_SET_SURFACE_FORMAT, static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (2u << 5) | (1u << 8));
        b.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
        b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, kZ24);
        b.method(m::NV4097_SET_SURFACE_PITCH_Z, kW * 4);
        b.method(m::NV4097_SET_DEPTH_TEST_ENABLE, 1);
        b.method(m::NV4097_SET_DEPTH_FUNC, static_cast<u32>(CompareFunc::Less));
        b.method(m::NV4097_SET_DEPTH_MASK, 0);
        set_fragment_program(b, kFp3Offset, 0x40);
        draw_triangle(b);
        REQUIRE(e->submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e->failures) INFO(f.message);
        CHECK(e->failures.empty());
        Result r;
        r.passes = e->backend->stats().render_passes;
        r.folded = e->backend->stats().clears_folded;
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, kW * kH * 4));
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kSurfaceB, kW * kH * 4));
        REQUIRE(e->backend->flush_memory(PS_MEM_RSX_LOCAL_BASE + kZ24, kW * kH * 4));
        r.a_region = guest_pixel(*e, kSurfaceOffset, 16, 40), r.a_out = guest_pixel(*e, kSurfaceOffset, 32, 60);
        r.b_region = guest_pixel(*e, kSurfaceB, 16, 40), r.b_out = guest_pixel(*e, kSurfaceB, 32, 60);
        r.z_region = e->local32(kZ24 + 40 * kW * 4 + 16 * 4), r.z_out = e->local32(kZ24 + 60 * kW * 4 + 32 * 4);
        return r;
    };
    for (const int order : {0, 1, 2}) {
        INFO(std::format("order {}", order));
        const Result on = run(true, order), off = run(false, order);
        CHECK(on.a_region == kRed); // A's own triangle: the clears of B leave it, and the draw fails the depth test there
        CHECK(on.a_out == kRed); // the draw, or A's own triangle where the depth test failed
        // Outside the region the draw passes the depth test only where nothing cleared the depth buffer (it started at
        // 1.0, as a buffer nobody wrote).
        CHECK(on.b_out == (order == 0 ? kGreen : kBlue));
        CHECK(on.b_region == (order == 1 ? kGreen : kBlue));
        CHECK(on.z_region == (order == 0 ? 0x40000055u : 0x40000033u));
        CHECK(on.z_out == (order == 0 ? 0xffffff00u : 0x40000033u));
        for (const auto& [x, y] : {std::pair{on.a_region, off.a_region}, {on.a_out, off.a_out},
                 {on.b_region, off.b_region}, {on.b_out, off.b_out}})
            CHECK(x == y);
        CHECK(on.z_region == off.z_region);
        CHECK(on.z_out == off.z_out);
        CHECK(on.passes <= off.passes);
        CHECK(on.folded >= 1);
    }
}

// ---- a frame shaped like GTA V's render passes, measured with and without D-945 -------------------------------------

namespace {

// Colour targets (A8R8G8B8, w x h each, pitch w * 4) and a depth buffer (Z24S8, or Z16) at local offsets; viewport and
// scissor over all of them.
struct BenchTargets {
    std::vector<u32> colors;
    u32 w = 0, h = 0;
    u32 depth = 0; // 0: none
    bool z16 = false;
};

void bench_targets(CommandBuilder& b, const BenchTargets& t)
{
    b.method(m::NV4097_SET_SURFACE_FORMAT,
        static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | ((t.z16 ? 1u : 2u) << 5) | (1u << 8));
    static constexpr std::array<u32, 4> kDma = {m::NV4097_SET_CONTEXT_DMA_COLOR_A, m::NV4097_SET_CONTEXT_DMA_COLOR_B,
        m::NV4097_SET_CONTEXT_DMA_COLOR_C, m::NV4097_SET_CONTEXT_DMA_COLOR_D};
    static constexpr std::array<u32, 4> kOffset = {m::NV4097_SET_SURFACE_COLOR_AOFFSET, m::NV4097_SET_SURFACE_COLOR_BOFFSET,
        m::NV4097_SET_SURFACE_COLOR_COFFSET, m::NV4097_SET_SURFACE_COLOR_DOFFSET};
    static constexpr std::array<u32, 4> kPitch = {m::NV4097_SET_SURFACE_PITCH_A, m::NV4097_SET_SURFACE_PITCH_B,
        m::NV4097_SET_SURFACE_PITCH_C, m::NV4097_SET_SURFACE_PITCH_D};
    static constexpr std::array<SurfaceTarget, 5> kTarget = {SurfaceTarget::None, SurfaceTarget::A, SurfaceTarget::AB,
        SurfaceTarget::ABC, SurfaceTarget::ABCD};
    for (usize i = 0; i < t.colors.size(); i++) {
        b.method(kDma[i], loc::kDmaFrameBuffer);
        b.method(kOffset[i], t.colors[i]);
        b.method(kPitch[i], t.w * 4);
    }
    b.method(m::NV4097_SET_SURFACE_COLOR_TARGET, static_cast<u32>(kTarget[t.colors.size()]));
    b.method(m::NV4097_SET_COLOR_MASK_MRT, 0xfff0);
    if (t.depth) {
        b.method(m::NV4097_SET_CONTEXT_DMA_ZETA, loc::kDmaFrameBuffer);
        b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, t.depth);
        b.method(m::NV4097_SET_SURFACE_PITCH_Z, t.w * (t.z16 ? 2 : 4));
    }
    b.method(m::NV4097_SET_SURFACE_CLIP_HORIZONTAL, {t.w << 16, t.h << 16});
    b.method(m::NV4097_SET_VIEWPORT_HORIZONTAL, {t.w << 16, t.h << 16});
    b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {t.w << 16, t.h << 16});
    const float hw = static_cast<float>(t.w) / 2.f, hh = static_cast<float>(t.h) / 2.f;
    b.method(m::NV4097_SET_VIEWPORT_OFFSET, {f2u(hw), f2u(hh), f2u(0.5f), f2u(0.f)});
    b.method(m::NV4097_SET_VIEWPORT_SCALE, {f2u(hw), f2u(-hh), f2u(0.5f), f2u(0.f)});
}

void bench_draws(CommandBuilder& b, u32 n)
{
    for (u32 i = 0; i < n; i++) draw_triangle(b);
}

// One frame, its passes in the order GTA V's prologue draws them (runs 291 and 305: D-270, D-273), every draw the same
// triangle. Synthetic: sizes and patterns only.
void bench_frame(CommandBuilder& b)
{
    constexpr u32 kG = 0x1000000, kZ = 0x2000000, kL = 0x2400000, kF = 0x2800000, kP = 0x2c00000;
    constexpr u32 kR = 0x3800000, kRZ = 0x3840000, kRO = 0x3880000, kS = 0x3a00000, kC0 = 0x3c00000, kC1 = 0x3c40000;
    const auto g = [](u32 i) { return kG + i * 0x400000; };
    const auto sample = [&](u32 offset, u32 w, u32 h) {
        set_fragment_program(b, kFp2Offset, 0x40);
        bind_linear_texture(b, offset, tex::A8R8G8B8, w * 4, w, h);
    };
    const auto colour = [&]() { set_fragment_program(b, kFpOffset, 0x40); };
    const auto depth_off = [&]() { b.method(m::NV4097_SET_DEPTH_TEST_ENABLE, 0); };
    b.method(m::NV4097_SET_STENCIL_MASK, 0xff);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff203040u);
    b.method(m::NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xffffff00u);
    // Reflections: a small view drawn, then copied down by a draw that samples it, six times.
    for (u32 i = 0; i < 6; i++) {
        bench_targets(b, {{kR}, 256, 128, kRZ});
        depth_test_less(b);
        colour();
        bench_draws(b, 1);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor | clear::kDepthStencil); // after a draw: a quad
        bench_draws(b, 8);
        bench_targets(b, {{kRO + i * 0x10000}, 128, 128});
        depth_off();
        sample(kR, 256, 128);
        bench_draws(b, 1);
    }
    // Small targets cleared, one of them only cleared.
    bench_targets(b, {{kC0}, 320, 128});
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    colour();
    bench_draws(b, 3);
    bench_targets(b, {{kC1}, 320, 128});
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    // The G-buffer: its first target cleared alone, the depth buffer alone, then all four with depth, switching to the
    // first alone in between.
    bench_targets(b, {{g(0)}, 1280, 720});
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    bench_targets(b, {{}, 1280, 720, kZ});
    b.method(m::NV4097_CLEAR_SURFACE, clear::kDepthStencil);
    const BenchTargets gbuffer{{g(0), g(1), g(2), g(3)}, 1280, 720, kZ}, first{{g(0)}, 1280, 720, kZ};
    bench_targets(b, gbuffer);
    depth_test_less(b);
    set_fragment_program(b, kFp3Offset, 0x40);
    bench_draws(b, 100);
    sample(kC1, 320, 128);
    bench_draws(b, 5);
    colour();
    bench_targets(b, first);
    bench_draws(b, 16);
    bench_targets(b, gbuffer);
    bench_draws(b, 17);
    bench_targets(b, first);
    bench_draws(b, 1);
    bench_targets(b, gbuffer);
    bench_draws(b, 33);
    // Shadow tiles: depth only.
    for (u32 i = 0; i < 8; i++) {
        bench_targets(b, {{}, 256, 256, kS + i * 0x20000, true});
        bench_draws(b, 10);
    }
    // Lighting, with a chain of small passes before it and a target cleared in part in between.
    const BenchTargets light{{kL}, 1280, 720, kZ};
    bench_targets(b, light);
    bench_draws(b, 2);
    depth_off();
    for (u32 i = 0; i < 4; i++) {
        bench_targets(b, {{kP + i * 0x100000}, 640, 360});
        sample(i ? kP + (i - 1) * 0x100000 : g(2), i ? 640 : 1280, i ? 360 : 720);
        bench_draws(b, 1);
    }
    bench_targets(b, light);
    depth_test_less(b);
    sample(g(0), 1280, 720);
    bench_draws(b, 50);
    bench_targets(b, {{kP}, 640, 360});
    b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {320u << 16, 320u << 16});
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor); // part of another target
    bench_targets(b, light);
    bench_draws(b, 10);
    bench_targets(b, {{}, 1280, 720, kZ});
    bench_draws(b, 1);
    bench_targets(b, light);
    bench_draws(b, 1);
    // Post-processing: each pass samples the one before.
    depth_off();
    static constexpr std::array<std::pair<u32, u32>, 4> kSizes = {{{640, 360}, {320, 180}, {160, 90}, {80, 45}}};
    for (u32 i = 0; i < 12; i++) {
        const auto [w, h] = kSizes[i % 4];
        const auto [pw, ph] = kSizes[(i + 3) % 4];
        bench_targets(b, {{kP + 0x400000 + (i % 4) * 0x100000}, w, h});
        sample(i ? kP + 0x400000 + ((i + 3) % 4) * 0x100000 : kL, i ? pw : 1280, i ? ph : 720);
        bench_draws(b, 1);
    }
    // The last passes: part of a target and of the stencil cleared, then drawn, then the frame.
    const auto region = [&] { b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {(200u << 16) | 86u, (144u << 16) | 525u}); };
    bench_targets(b, {{g(1)}, 1280, 720, kZ});
    region();
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor | clear::kDepthStencil);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kStencil);
    b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, {1280u << 16, 720u << 16});
    depth_test_less(b);
    colour();
    bench_draws(b, 40);
    bench_targets(b, {{g(1)}, 1280, 720});
    depth_off();
    bench_draws(b, 1);
    bench_targets(b, {{kF}, 1280, 720});
    sample(g(1), 1280, 720);
    bench_draws(b, 1);
    bench_targets(b, {{}, 1280, 720, kZ}); // part of the stencil again, between two passes over the frame
    region();
    b.method(m::NV4097_CLEAR_SURFACE, clear::kStencil);
    bench_targets(b, {{kF}, 1280, 720, kZ});
    depth_test_less(b);
    colour();
    bench_draws(b, 1);
    bench_targets(b, {{kF}, 1280, 720});
    depth_off();
    sample(kL, 1280, 720);
    bench_draws(b, 14);
    b.method(m::GCM_FLIP_COMMAND, 0);
}

} // namespace

TEST_CASE("metal bench: render passes, attachment traffic and GPU time of a frame shaped like GTA V's (D-945)", "[.bench]")
{
    // Synthetic: GTA V's frame structure (passes, sizes, clears), not its shaders; a measuring aid for D-945, run alone:
    // test_rsx_metal "[.bench]" (PSPOCKET_BENCH_FRAMES frames each way, 600 by default).
    REQUIRE_METAL();
    const char* frames_env = std::getenv("PSPOCKET_BENCH_FRAMES");
    const u32 frames = std::max(300, frames_env ? std::atoi(frames_env) : 600);
    for (const bool merging : {false, true, false, true}) {
        std::vector<metal::WindowStats> windows;
        g_e2e_config = [&](MetalBackendConfig& c) {
            c.merge_passes = merging;
            c.present = nullptr;
            c.on_window = [&](const metal::WindowStats& w) { windows.push_back(w); };
        };
        E2E e;
        g_e2e_config = nullptr;
        REQUIRE(e.mem.commit(PS_MEM_RSX_LOCAL_BASE + 0x1000000, 0x3000000));
        e.put_local_bytes(kFpOffset, fp_color(false));
        e.put_local_bytes(kFp2Offset, fp_texture());
        e.put_local_bytes(kFp3Offset, fp_two_targets());
        write_triangle(e, {0xff0000ff, 0x00ff00ff, 0x0000ffff});
        e.rsx->set_display_buffer(0, {0x2800000, 1280 * 4, 1280, 720});
        auto b = e.at(0);
        upload_vertex_program(b);
        bind_vertex_arrays(b);
        bench_frame(b);
        for (u32 f = 0; f < frames; f++) {
            e.rsx->set_fifo_pointers(0, 0);
            REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        }
        e.backend->sync();
        for (const auto& fl : e.failures) UNSCOPED_INFO(fl.message);
        CHECK(e.failures.empty());
        REQUIRE(windows.size() >= 1);
        const metal::WindowStats& w = windows.back();
        std::string ends;
        for (usize k = 0; k < w.pass_ends.size(); k++)
            if (w.pass_ends[k] > 0) ends += std::format("{}{} {:.1f}", ends.empty() ? "" : ", ", metal::kPassEndNames[k], w.pass_ends[k]);
        std::printf("D-945 bench, %s: GPU %.2f ms a frame, %.1f render passes (%s), attachments %.1f MB loaded, %.1f MB "
                    "stored, %.1f MB not stored; %.1f clears folded, %.1f let a pass go on\n",
            merging ? "merging" : "as before", w.gpu_ms, w.render_passes, ends.c_str(), w.attachment_load_mb,
            w.attachment_store_mb, w.attachment_store_skipped_mb, w.clears_folded, w.clears_waited);
    }
    // In one backend, the change on in every other run of 10 frames (PSPOCKET_GPU_AB, D-272): both halves see the same
    // load on the Mac. The backend logs each half's GPU time, passes and traffic every 200 frames ("GPU A/B").
    setenv("PSPOCKET_GPU_AB", "10", 1);
    g_e2e_config = [&](MetalBackendConfig& c) { c.present = nullptr; };
    E2E e;
    g_e2e_config = nullptr;
    unsetenv("PSPOCKET_GPU_AB");
    REQUIRE(e.mem.commit(PS_MEM_RSX_LOCAL_BASE + 0x1000000, 0x3000000));
    e.put_local_bytes(kFpOffset, fp_color(false));
    e.put_local_bytes(kFp2Offset, fp_texture());
    e.put_local_bytes(kFp3Offset, fp_two_targets());
    write_triangle(e, {0xff0000ff, 0x00ff00ff, 0x0000ffff});
    e.rsx->set_display_buffer(0, {0x2800000, 1280 * 4, 1280, 720});
    auto b = e.at(0);
    upload_vertex_program(b);
    bind_vertex_arrays(b);
    bench_frame(b);
    for (u32 f = 0; f < 2 * frames; f++) {
        e.rsx->set_fifo_pointers(0, 0);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    }
    e.backend->sync();
    CHECK(e.failures.empty());
}

namespace {

// Frames for the capture round trip: a clear, then a triangle whose colours the "CPU" changes between frames, a flip.
u32 capture_frame(E2E& e, u32 at, u32 clear, u32 color)
{
    write_triangle(e, {color, color, color});
    auto b = e.at(at);
    setup_surface(b, kSurfaceOffset);
    b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, clear);
    b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
    upload_vertex_program(b);
    set_fragment_program(b, kFpOffset, 0x40);
    bind_vertex_arrays(b);
    draw_triangle(b);
    b.method(m::GCM_FLIP_COMMAND, 0);
    REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
    return b.offset();
}

std::string capture_dir(const char* name)
{
    const auto dir = std::filesystem::temp_directory_path() / std::format("pspocket-capture-{}-{}", name, getpid());
    std::filesystem::remove_all(dir);
    return dir.string();
}

} // namespace

TEST_CASE("rsx capture: frames captured mid-run replay to the same pixels, every time (D-1053)")
{
    REQUIRE_METAL();
    const std::string dir = capture_dir("roundtrip");
    std::vector<std::vector<u8>> captured;
    {
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        e.rsx->set_display_buffer(0, {kSurfaceOffset, kW * 4, kW, kH});
        CaptureOptions o;
        o.dir = dir;
        o.from_flip = 1; // after the first frame: the capture starts with a target the GPU drew
        o.frames = 2;
        CaptureWriter w(e.mem, *e.tracker, o);
        e.rsx->set_capture(&w);
        u32 at = capture_frame(e, 0, 0xff102030, 0xff0000ff);
        at = capture_frame(e, at, 0xff203040, 0xff00ff00);
        at = capture_frame(e, at, 0xff304050, 0xffff0000);
        capture_frame(e, at, 0xff405060, 0xffffffff); // after the capture's two frames: not in it
        e.rsx->set_capture(nullptr);
        INFO(w.summary());
        CHECK(w.phase() == CaptureWriter::Phase::Done);
        e.backend->sync();
        REQUIRE(e.present_pixels.size() == 4);
        captured = {e.present_pixels[1], e.present_pixels[2]};
    }
    CaptureInfo info;
    std::string error;
    REQUIRE(read_capture_info(dir, info, &error));
    CHECK(info.complete);
    CHECK(info.frames == 2);
    for (int run = 0; run < 2; run++) {
        INFO("replay " << run);
        E2E r;
        ReplayResult result;
        REQUIRE(replay_capture(dir, r.mem, *r.tracker, *r.rsx, {}, result, &error));
        INFO(error << " " << result.first_deviation);
        CHECK(result.deviations == 0);
        CHECK(result.flips == 2);
        r.backend->sync();
        REQUIRE(r.present_pixels.size() == 2);
        CHECK(r.present_pixels[0] == captured[0]);
        CHECK(r.present_pixels[1] == captured[1]);
        for (const auto& f : r.failures) UNSCOPED_INFO(f.message);
        CHECK(r.failures.empty());
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("rsx capture: a capture past its size cap stops, its manifest incomplete (D-1053)")
{
    REQUIRE_METAL();
    const std::string dir = capture_dir("cap");
    E2E e;
    e.put_local_bytes(kFpOffset, fp_color(false));
    e.rsx->set_display_buffer(0, {kSurfaceOffset, kW * 4, kW, kH});
    for (u32 i = 0; i < 64; i++) fill_random(e.mem, PS_MEM_RSX_LOCAL_BASE + 0x400000 + i * 0x10000, 0x10000, i);
    CaptureOptions o;
    o.dir = dir;
    o.from_flip = 0;
    o.frames = 5;
    o.max_bytes = 1u << 20; // the 4 MB of random pages alone pass it
    CaptureWriter w(e.mem, *e.tracker, o);
    e.rsx->set_capture(&w);
    const u32 at = capture_frame(e, 0, 0xff102030, 0xff0000ff); // the FIFO's setup taken: the capture starts next
    capture_frame(e, at, 0xff203040, 0xff00ff00);
    e.rsx->set_capture(nullptr);
    INFO(w.summary());
    CHECK(w.phase() == CaptureWriter::Phase::Failed);
    CHECK(w.summary().find("cap") != std::string::npos);
    CaptureInfo info;
    std::string error;
    REQUIRE(read_capture_info(dir, info, &error));
    CHECK_FALSE(info.complete);
    std::filesystem::remove_all(dir);
}

TEST_CASE("rsx capture: an unarmed capture waits for its trigger, then records the frames after it (D-1054)")
{
    REQUIRE_METAL();
    const std::string dir = capture_dir("armed");
    std::vector<u8> after_trigger;
    {
        E2E e;
        e.put_local_bytes(kFpOffset, fp_color(false));
        e.rsx->set_display_buffer(0, {kSurfaceOffset, kW * 4, kW, kH});
        CaptureOptions o;
        o.dir = dir;
        o.frames = 1;
        o.armed = false; // pspocket-boot --rsx-capture-on: a detector arms it
        CaptureWriter w(e.mem, *e.tracker, o);
        e.rsx->set_capture(&w);
        u32 at = capture_frame(e, 0, 0xff102030, 0xff0000ff);
        at = capture_frame(e, at, 0xff203040, 0xff00ff00);
        CHECK(w.phase() == CaptureWriter::Phase::Waiting); // two flips past from_flip 0: still waiting
        CHECK(w.arm(0));
        CHECK_FALSE(w.arm(0)); // once only
        at = capture_frame(e, at, 0xff304050, 0xffff0000);
        capture_frame(e, at, 0xff405060, 0xffffffff);
        e.rsx->set_capture(nullptr);
        INFO(w.summary());
        CHECK(w.phase() == CaptureWriter::Phase::Done);
        CHECK(w.summary().find("from flip 2") != std::string::npos);
        e.backend->sync();
        REQUIRE(e.present_pixels.size() == 4);
        after_trigger = e.present_pixels[2];
    }
    CaptureInfo info;
    std::string error;
    REQUIRE(read_capture_info(dir, info, &error));
    CHECK(info.complete);
    CHECK(info.from_flip == 2);
    E2E r;
    ReplayResult result;
    REQUIRE(replay_capture(dir, r.mem, *r.tracker, *r.rsx, {}, result, &error));
    CHECK(result.deviations == 0);
    r.backend->sync();
    REQUIRE(r.present_pixels.size() == 1);
    CHECK(r.present_pixels[0] == after_trigger);
    std::filesystem::remove_all(dir);
}

TEST_CASE("PSPOCKET_RSX_WRITEBACK_AT writes back the targets over its ranges after its flip, and no others (D-1055)")
{
    REQUIRE_METAL();
    const auto clears_in_memory = [](E2E& e, u32 value) {
        u32 n = 0;
        for (u32 i = 0; i < kW * kH; i++) n += e.local32(kSurfaceOffset + i * 4) == value;
        return n;
    };
    for (const bool over_target : {true, false}) {
        INFO("the range " << (over_target ? "holds" : "misses") << " the target");
        const std::string spec = over_target
            ? std::format("1:{:#x}-{:#x}", PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset, PS_MEM_RSX_LOCAL_BASE + kSurfaceOffset + 1)
            : std::format("1:{:#x}-{:#x}", PS_MEM_RSX_LOCAL_BASE + 0x800000, PS_MEM_RSX_LOCAL_BASE + 0x800001);
        ::setenv("PSPOCKET_RSX_WRITEBACK_AT", spec.c_str(), 1);
        E2E e;
        ::unsetenv("PSPOCKET_RSX_WRITEBACK_AT");
        e.put_local_bytes(kFpOffset, fp_color(false));
        e.rsx->set_display_buffer(0, {kSurfaceOffset, kW * 4, kW, kH});
        const u32 at = capture_frame(e, 0, 0xff102030, 0xff0000ff);
        CHECK(clears_in_memory(e, 0xff102030) == 0); // drawn on the GPU, not in memory
        capture_frame(e, at, 0xff203040, 0xff00ff00); // the step after flip 1 writes back the first frame's target
        e.backend->sync();
        if (over_target) CHECK(clears_in_memory(e, 0xff102030) > 0);
        else CHECK(clears_in_memory(e, 0xff102030) == 0);
    }
}

TEST_CASE("metal e2e: an index array the GPU drew is written back before its indices are read; without, the alarm "
          "counts it (D-1056)")
{
    // Render to index buffer: a target's memory read as an index array. The indices are scanned on the CPU (the vertex
    // range), so a GPU-newer target there goes to memory first.
    REQUIRE_METAL();
    for (const bool sync : {true, false}) {
        INFO((sync ? "synced" : "PSPOCKET_RSX_NO_STALE_SYNC"));
        if (!sync) ::setenv("PSPOCKET_RSX_NO_STALE_SYNC", "1", 1);
        E2E e;
        ::unsetenv("PSPOCKET_RSX_NO_STALE_SYNC");
        e.put_local_bytes(kFpOffset, fp_color(false));
        write_triangle(e, {0xff0000ff, 0xff0000ff, 0xff0000ff});
        auto b = e.at(0);
        setup_surface(b, kSurfaceB); // the index buffer's memory, drawn by the GPU: words 0x00000001, indices 0 and 1
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0x00000001);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        setup_surface(b, kSurfaceOffset);
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        upload_vertex_program(b);
        set_fragment_program(b, kFpOffset, 0x40);
        bind_vertex_arrays(b);
        b.method(m::NV4097_SET_INDEX_ARRAY_ADDRESS, {kSurfaceB, loc::kLocal | (1u << 4)}); // 16-bit indices
        b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::Triangles));
        b.method(m::NV4097_DRAW_INDEX_ARRAY, (2u << 24) | 0);
        b.method(m::NV4097_SET_BEGIN_END, 0);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        CHECK(e.backend->stats().stale_synced == (sync ? 1u : 0u));
        CHECK(e.rsx->alarms().get(Alarm::StaleGpuRead) == (sync ? 0u : 1u));
        CHECK(e.local32(kSurfaceB) == (sync ? 0x00000001u : 0u)); // what the indices were read from
    }
}

TEST_CASE("metal e2e: a display buffer shown from memory gets what a target inside it drew; without the sync, the "
          "alarm counts it (D-1056)")
{
    // No target at the display buffer's start: the flip shows memory, with a target drawn over its lower half (the
    // same pitch, 32 rows in) written back first.
    REQUIRE_METAL();
    for (const bool sync : {true, false}) {
        INFO((sync ? "synced" : "PSPOCKET_RSX_NO_STALE_SYNC"));
        if (!sync) ::setenv("PSPOCKET_RSX_NO_STALE_SYNC", "1", 1);
        E2E e;
        ::unsetenv("PSPOCKET_RSX_NO_STALE_SYNC");
        e.rsx->set_display_buffer(0, {kSurfaceB, kW * 4, kW, kH});
        auto b = e.at(0);
        setup_surface(b, kSurfaceB + 32 * kW * 4); // rows 32-95 of the buffer's memory: its lower half and beyond
        b.method(m::NV4097_SET_COLOR_CLEAR_VALUE, 0xff00ff00);
        b.method(m::NV4097_CLEAR_SURFACE, clear::kColor);
        b.method(m::GCM_FLIP_COMMAND, 0);
        REQUIRE(e.submit(b) == Rsx::RunStatus::Idle);
        e.backend->sync();
        for (const auto& f : e.failures) UNSCOPED_INFO(f.message);
        CHECK(e.failures.empty());
        CHECK(e.rsx->alarms().get(Alarm::StaleGpuRead) == (sync ? 0u : 1u));
        REQUIRE(e.present_pixels.size() == 1);
        const std::vector<u8>& px = e.present_pixels[0]; // BGRA, 128x128
        const auto at = [&](u32 x, u32 y) { return std::array<u8, 3>{px[(y * 128 + x) * 4], px[(y * 128 + x) * 4 + 1],
                                                px[(y * 128 + x) * 4 + 2]}; };
        CHECK(at(64, 32) == (std::array<u8, 3>{0, 0, 0}));                           // the upper half: memory, zeros
        CHECK(at(64, 96) == (sync ? std::array<u8, 3>{0, 0xff, 0} : std::array<u8, 3>{0, 0, 0})); // the target's green
    }
}
