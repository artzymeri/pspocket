// State tracking: decoded views of the register file after synthetic method streams (surfaces, viewport, scissor,
// blend, depth/stencil, raster, vertex arrays, index array, textures, shader program), vertex program upload,
// transform constants, immediate vertex attributes and draw clauses.

#include <catch_amalgamated.hpp>

#include "pspocket/rsx/method_names.hpp"
#include "rsx_test_util.hpp"

using namespace pspocket;
using namespace pspocket::rsx;
using namespace pspocket::rsx::test;
using Kind = RecordingBackend::Kind;

TEST_CASE("reset state matches the cellGcmInit defaults")
{
    RsxState s;
    const SurfaceInfo surf = s.surface();
    CHECK(surf.color_format == 1);  // 0x121: X1R5G5B5_Z1R5G5B5
    CHECK(surf.depth_format == 1);
    CHECK(surf.type == 1);
    CHECK(surf.target == static_cast<u32>(SurfaceTarget::A));
    CHECK(s.depth_stencil().depth_func == static_cast<u32>(CompareFunc::Less));
    CHECK(s.depth_stencil().depth_write);
    CHECK(s.color_mask().mask[0] == 0xf);
    CHECK(s.blend().equation_rgb == static_cast<u32>(BlendEquation::Add));
    CHECK(s.blend().sfactor_rgb == static_cast<u32>(BlendFactor::One));
    CHECK(s.blend().dfactor_rgb == static_cast<u32>(BlendFactor::Zero));
    CHECK(s.viewport().scale[0] == 2048.f);
    CHECK(s.viewport().offset[2] == 0.5f);
    CHECK(s.vertex_attrib_input_mask() == 0xffff);
    CHECK(s.fragment_texture(3).remap == tex::kRemapIdentity);
    CHECK(s.shader_program().location == 0);
}

TEST_CASE("surface state decodes formats, targets, offsets and clip")
{
    Fixture f;
    auto b = f.at(0);
    b.method(m::NV4097_SET_SURFACE_FORMAT,
        static_cast<u32>(SurfaceColorFormat::A8R8G8B8) | (2u << 5) | (1u << 8) | (10u << 16) | (9u << 24));
    b.method(m::NV4097_SET_CONTEXT_DMA_COLOR_A, loc::kDmaFrameBuffer);
    b.method(m::NV4097_SET_SURFACE_COLOR_AOFFSET, 0x100000);
    b.method(m::NV4097_SET_SURFACE_PITCH_A, 1280 * 4);
    b.method(m::NV4097_SET_CONTEXT_DMA_COLOR_B, loc::kDmaHostBuffer);
    b.method(m::NV4097_SET_SURFACE_COLOR_BOFFSET, 0x200000);
    b.method(m::NV4097_SET_SURFACE_PITCH_B, 512);
    b.method(m::NV4097_SET_SURFACE_ZETA_OFFSET, 0x400000);
    b.method(m::NV4097_SET_SURFACE_PITCH_Z, 1280 * 4);
    b.method(m::NV4097_SET_SURFACE_COLOR_TARGET, static_cast<u32>(SurfaceTarget::AB));
    b.method(m::NV4097_SET_SURFACE_CLIP_HORIZONTAL, {(1280u << 16) | 0, (720u << 16) | 0});
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);

    const SurfaceInfo s = f.state().surface();
    CHECK(s.color_format == static_cast<u32>(SurfaceColorFormat::A8R8G8B8));
    CHECK(s.depth_format == static_cast<u32>(SurfaceDepthFormat::Z24S8));
    CHECK(s.type == 1);
    CHECK(s.log2_width == 10);
    CHECK(s.log2_height == 9);
    CHECK(s.clip_width == 1280);
    CHECK(s.clip_height == 720);
    CHECK(s.color[0].offset == 0x100000);
    CHECK(s.color[0].pitch == 5120);
    CHECK(s.color[0].dma == loc::kDmaFrameBuffer);
    CHECK(s.color[1].dma == loc::kDmaHostBuffer);
    CHECK(s.depth.offset == 0x400000);
    std::array<u32, kMaxColorTargets> targets{};
    REQUIRE(s.active_targets(targets) == 2);
    CHECK(targets[0] == 0);
    CHECK(targets[1] == 1);
    CHECK((f.state().dirty_bits & dirty::kSurface) != 0);

    SurfaceInfo only_b = s;
    only_b.target = static_cast<u32>(SurfaceTarget::B);
    REQUIRE(only_b.active_targets(targets) == 1);
    CHECK(targets[0] == 1);
}

TEST_CASE("viewport, scissor and shader window")
{
    Fixture f;
    auto b = f.at(0);
    b.method(m::NV4097_SET_VIEWPORT_HORIZONTAL, {(1152u << 16) | 8, (640u << 16) | 4});
    b.method(m::NV4097_SET_SCISSOR_HORIZONTAL, (100u << 16) | 10);
    b.method(m::NV4097_SET_SCISSOR_VERTICAL, (50u << 16) | 20);
    b.method(m::NV4097_SET_VIEWPORT_OFFSET, {f2u(576.f), f2u(320.f), f2u(0.5f), f2u(0.f)});
    b.method(m::NV4097_SET_VIEWPORT_SCALE, {f2u(576.f), f2u(-320.f), f2u(0.5f), f2u(0.f)});
    b.method(m::NV4097_SET_CLIP_MIN, {f2u(0.f), f2u(1.f)});
    b.method(m::NV4097_SET_SHADER_WINDOW, 640 | (1u << 12) | (1u << 16));
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);

    const ViewportInfo v = f.state().viewport();
    CHECK(v.x == 8);
    CHECK(v.width == 1152);
    CHECK(v.y == 4);
    CHECK(v.height == 640);
    CHECK(v.scale[1] == -320.f);
    CHECK(v.offset[0] == 576.f);
    CHECK(v.clip_max == 1.f);
    const ScissorInfo s = f.state().scissor();
    CHECK(s.x == 10);
    CHECK(s.width == 100);
    CHECK(s.y == 20);
    CHECK(s.height == 50);
    const ShaderWindow w = f.state().shader_window();
    CHECK(w.height == 640);
    CHECK(w.origin_bottom);
    CHECK(w.pixel_center_integer);
}

TEST_CASE("blend, depth, stencil, cull and color mask")
{
    Fixture f;
    auto b = f.at(0);
    b.method(m::NV4097_SET_BLEND_ENABLE, 1);
    b.method(m::NV4097_SET_BLEND_FUNC_SFACTOR, {
        static_cast<u32>(BlendFactor::SrcAlpha) | (static_cast<u32>(BlendFactor::One) << 16),
        static_cast<u32>(BlendFactor::OneMinusSrcAlpha) | (static_cast<u32>(BlendFactor::Zero) << 16),
        0x80402010, // BLEND_COLOR
        static_cast<u32>(BlendEquation::Add) | (static_cast<u32>(BlendEquation::Max) << 16),
        0x01000100, // COLOR_MASK: A and G only
    });
    b.method(m::NV4097_SET_BLEND_ENABLE_MRT, 0x4); // target C
    b.method(m::NV4097_SET_COLOR_MASK_MRT, 0x0a50); // target 1: R+B (0x5 -> bits A,G? see below), target 2: ...
    b.method(m::NV4097_SET_DEPTH_FUNC, {0x206, 0, 1}); // GEQUAL, no write, test on
    b.method(m::NV4097_SET_STENCIL_TEST_ENABLE, {1, 0x0f, 0x203, 0x80, 0x7f, 0x1e01, 0x8507, 0x150a});
    b.method(m::NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE, {1, 0x3, 0x207, 0x1, 0xff, 0x1e00, 0x1e02, 0x0});
    b.method(m::NV4097_SET_CULL_FACE_ENABLE, 1);
    b.method(m::NV4097_SET_CULL_FACE, static_cast<u32>(CullFace::Front));
    b.method(m::NV4097_SET_FRONT_FACE, static_cast<u32>(FrontFace::CW));
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.empty());

    const BlendInfo bl = f.state().blend();
    CHECK(bl.enable);
    CHECK(bl.enabled_for(0));
    CHECK_FALSE(bl.enabled_for(1));
    CHECK(bl.enabled_for(2));
    CHECK(bl.sfactor_rgb == static_cast<u32>(BlendFactor::SrcAlpha));
    CHECK(bl.sfactor_a == static_cast<u32>(BlendFactor::One));
    CHECK(bl.dfactor_rgb == static_cast<u32>(BlendFactor::OneMinusSrcAlpha));
    CHECK(bl.equation_a == static_cast<u32>(BlendEquation::Max));
    CHECK(bl.color == 0x80402010);

    const ColorMaskInfo cm = f.state().color_mask();
    CHECK(cm.mask[0] == (2 | 8)); // G and A
    // MRT word 0x0a50: target 1 nibble 0x5 = A(bit0) + G(bit2); target 2 nibble 0xa = R(bit1) + B(bit3)
    CHECK(cm.mask[1] == (8 | 2));
    CHECK(cm.mask[2] == (1 | 4));

    const DepthStencilInfo ds = f.state().depth_stencil();
    CHECK(ds.depth_test);
    CHECK_FALSE(ds.depth_write);
    CHECK(ds.depth_func == static_cast<u32>(CompareFunc::GEqual));
    CHECK(ds.stencil_test);
    CHECK(ds.front.write_mask == 0x0f);
    CHECK(ds.front.func == static_cast<u32>(CompareFunc::LEqual));
    CHECK(ds.front.ref == 0x80);
    CHECK(ds.front.func_mask == 0x7f);
    CHECK(ds.front.op_fail == static_cast<u32>(StencilOp::Replace));
    CHECK(ds.front.op_zfail == static_cast<u32>(StencilOp::IncrWrap));
    CHECK(ds.front.op_zpass == static_cast<u32>(StencilOp::Invert));
    CHECK(ds.two_sided_stencil);
    CHECK(ds.back.write_mask == 0x3);
    CHECK(ds.back.func == static_cast<u32>(CompareFunc::Always));
    CHECK(ds.back.op_zfail == static_cast<u32>(StencilOp::Incr));
    CHECK(ds.back.op_zpass == static_cast<u32>(StencilOp::Zero));

    const RasterInfo r = f.state().raster();
    CHECK(r.cull_enable);
    CHECK(r.cull_face == static_cast<u32>(CullFace::Front));
    CHECK(r.front_face == static_cast<u32>(FrontFace::CW));
    CHECK(r.line_width == 1.f);
}

TEST_CASE("vertex arrays, index array and frequency dividers")
{
    Fixture f;
    auto b = f.at(0);
    // attr 0: float3, stride 12, local offset 0x1000; attr 3: ub4 (normalised), stride 16, main memory, frequency 4
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0, static_cast<u32>(VertexType::F) | (3u << 4) | (12u << 8));
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0, 0x1000);
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3,
        static_cast<u32>(VertexType::UB) | (4u << 4) | (16u << 8) | (4u << 16));
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3, 0x80000000u | 0x2000);
    b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 5, 0); // type 0 decodes as UB256
    b.method(m::NV4097_SET_FREQUENCY_DIVIDER_OPERATION, 1u << 3);
    b.method(m::NV4097_SET_VERTEX_DATA_BASE_OFFSET, {0x40, 7});
    b.method(m::NV4097_SET_INDEX_ARRAY_ADDRESS, {0x3000, loc::kMain | (1u << 4)});
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.empty());

    const VertexArrayInfo a0 = f.state().vertex_array(0);
    CHECK(a0.enabled());
    CHECK(a0.type == VertexType::F);
    CHECK(a0.size == 3);
    CHECK(a0.stride == 12);
    CHECK(a0.offset == 0x1000);
    CHECK(a0.location == 0);
    const VertexArrayInfo a3 = f.state().vertex_array(3);
    CHECK(a3.type == VertexType::UB);
    CHECK(a3.size == 4);
    CHECK(a3.frequency == 4);
    CHECK(a3.location == 1);
    CHECK(a3.offset == 0x2000);
    CHECK(f.state().vertex_array(5).type == VertexType::UB256);
    CHECK_FALSE(f.state().vertex_array(1).enabled());
    CHECK(f.state().frequency_divider_mask() == 8);
    CHECK(f.state().vertex_data_base_offset() == 0x40);
    CHECK(f.state().vertex_data_base_index() == 7);
    const IndexArrayInfo ia = f.state().index_array();
    CHECK(ia.offset == 0x3000);
    CHECK(ia.location == 1);
    CHECK(ia.u16_indices);
}

TEST_CASE("fragment and vertex texture descriptors")
{
    Fixture f;
    auto b = f.at(0);
    const u32 t = 2;
    b.method(m::NV4097_SET_TEXTURE_OFFSET + t * 8, {
        0x00123400,                                                                       // OFFSET
        (loc::kMain + 1) | (1u << 2) | (2u << 4) | ((tex::COMPRESSED_DXT45 | tex::kLinearFlag) << 8) | (5u << 16), // FORMAT
        1u | (3u << 8) | (5u << 16) | (0x2u << 12),                                       // ADDRESS
        (1u << 31) | (0x100u << 19) | (0x400u << 7) | (2u << 4) | (1u << 2),              // CONTROL0
        0x0000e4aa,                                                                       // CONTROL1 (remap)
        0x1ff8u | (6u << 16) | (2u << 24) | (0x9u << 28),                                 // FILTER (bias -0.03125)
        (256u << 16) | 128,                                                               // IMAGE_RECT
        0xff00ff00,                                                                       // BORDER_COLOR
    });
    b.method(m::NV4097_SET_TEXTURE_CONTROL3 + t, 1024 | (1u << 20));
    b.method(m::NV4097_SET_VERTEX_TEXTURE_OFFSET + 8, {0x5000,
        (loc::kLocal + 1) | (2u << 4) | ((tex::X32_FLOAT | tex::kLinearFlag) << 8) | (1u << 16)});
    b.method(m::NV4097_SET_VERTEX_TEXTURE_IMAGE_RECT + 8, (64u << 16) | 32);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.empty());

    const TextureInfo ti = f.state().fragment_texture(t);
    CHECK(ti.offset == 0x00123400);
    CHECK(ti.location == 1);
    CHECK(ti.cubemap);
    CHECK(ti.dimension == 2);
    CHECK(ti.base_format() == tex::COMPRESSED_DXT45);
    CHECK(ti.is_linear());
    CHECK_FALSE(ti.is_unnormalized());
    CHECK(ti.mipmaps == 5);
    CHECK(ti.wrap_s == 1);
    CHECK(ti.wrap_t == 3);
    CHECK(ti.wrap_r == 5);
    CHECK(ti.unsigned_remap == 2);
    CHECK(ti.enabled);
    CHECK(ti.min_lod == 1.f);
    CHECK(ti.max_lod == 4.f);
    CHECK(ti.max_aniso == 2);
    CHECK(ti.alpha_kill);
    CHECK(ti.remap == 0xe4aa);
    CHECK(ti.bias == -0.03125f);
    CHECK(ti.min_filter == 6);
    CHECK(ti.mag_filter == 2);
    CHECK(ti.argb_signed == 0x9);
    CHECK(ti.width == 256);
    CHECK(ti.height == 128);
    CHECK(ti.depth == 1); // CONTROL3 depth only counts for 3D textures
    CHECK(ti.pitch == 1024);
    CHECK(ti.border_color == 0xff00ff00);
    CHECK((f.state().dirty_bits & dirty::kFragmentTextures) != 0);

    const TextureInfo vt = f.state().vertex_texture(1);
    CHECK(vt.offset == 0x5000);
    CHECK(vt.location == 0);
    CHECK(vt.base_format() == tex::X32_FLOAT);
    CHECK(vt.width == 64);
    CHECK(vt.height == 32);
    CHECK(vt.remap == tex::kRemapIdentity);
}

TEST_CASE("vertex program upload advances the load pointer per instruction")
{
    Fixture f;
    auto b = f.at(0);
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 10);
    std::vector<u32> words;
    for (u32 i = 0; i < 12; i++) words.push_back(0x1000 + i); // three instructions
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM, words);
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 10);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    for (u32 i = 0; i < 12; i++) CHECK(f.state().transform_program[40 + i] == 0x1000 + i);
    CHECK(f.state().transform_program[39] == 0);
    CHECK(f.state().transform_program_load() == 13);
    CHECK(f.state().transform_program_start() == 10);

    // A second batch continues where the first stopped.
    auto c = f.at(b.offset());
    c.method(m::NV4097_SET_TRANSFORM_PROGRAM, {1, 2, 3, 4});
    REQUIRE(f.submit(c) == Rsx::RunStatus::Idle);
    CHECK(f.state().transform_program[52] == 1);
    CHECK(f.state().transform_program[55] == 4);
    CHECK(f.state().transform_program_load() == 14);
}

TEST_CASE("transform constants load at the constant load pointer")
{
    Fixture f;
    auto b = f.at(0);
    b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 466);
    b.method(m::NV4097_SET_TRANSFORM_CONSTANT,
        {f2u(1.f), f2u(2.f), f2u(3.f), f2u(4.f), f2u(5.f), f2u(6.f), f2u(7.f), f2u(8.f), f2u(9.f), f2u(10.f)});
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    CHECK(f.state().transform_constants[466] == float4{1.f, 2.f, 3.f, 4.f});
    CHECK(f.state().transform_constants[467] == float4{5.f, 6.f, 7.f, 8.f});
    CHECK(f.state().transform_constants[468] == float4{0.f, 0.f, 0.f, 0.f}); // c[468] does not exist: ignored
}

TEST_CASE("immediate vertex attributes decode every SET_VERTEX_DATA format")
{
    Fixture f;
    auto b = f.at(0);
    b.method(m::NV4097_SET_VERTEX_DATA4F_M + 4 * 1, {f2u(1.f), f2u(2.f), f2u(3.f), f2u(4.f)});
    b.method(m::NV4097_SET_VERTEX_DATA3F_M + 4 * 2, {f2u(5.f), f2u(6.f), f2u(7.f)});
    b.method(m::NV4097_SET_VERTEX_DATA2F_M + 2 * 3, {f2u(8.f), f2u(9.f)});
    b.method(m::NV4097_SET_VERTEX_DATA1F_M + 4, f2u(10.f));
    b.method(m::NV4097_SET_VERTEX_DATA4UB_M + 5, 0xff80ff00); // x = 0x00, y = 0xff, z = 0x80, w = 0xff
    b.method(m::NV4097_SET_VERTEX_DATA2S_M + 6, 0xfffe0003);  // x = 3, y = -2
    b.method(m::NV4097_SET_VERTEX_DATA4S_M + 2 * 7, {0x00020001, 0x00040003});
    b.method(m::NV4097_SET_VERTEX_DATA_SCALED4S_M + 2 * 8, {0x80007fff, 0x00000000});
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    const auto& v = f.state().vertex_registers;
    CHECK(v[1].value == float4{1.f, 2.f, 3.f, 4.f});
    CHECK(v[1].size == 4);
    CHECK(v[2].value == float4{5.f, 6.f, 7.f, 1.f});
    CHECK(v[3].value == float4{8.f, 9.f, 0.f, 1.f});
    CHECK(v[4].value == float4{10.f, 0.f, 0.f, 1.f});
    CHECK(v[5].value[0] == 0.f);
    CHECK(v[5].value[1] == 1.f);
    CHECK(v[5].value[2] == Catch::Approx(128.f / 255.f));
    CHECK(v[6].value[0] == 3.f);
    CHECK(v[6].value[1] == -2.f);
    CHECK(v[7].value == float4{1.f, 2.f, 3.f, 4.f});
    CHECK(v[8].value[0] == 1.f);  // 32767 -> 1
    CHECK(v[8].value[1] == -1.f); // -32768 -> -1
    CHECK(v[8].type == VertexType::S1);
    CHECK(f.backend.events.empty()); // no draw outside BEGIN/END
}

TEST_CASE("draw clauses: arrays, indexed, inline arrays, array elements and immediate mode")
{
    Fixture f;
    auto b = f.at(0);
    b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::Triangles));
    b.method(m::NV4097_DRAW_ARRAYS, (2u << 24) | 0);   // vertices 0-2
    b.method(m::NV4097_DRAW_ARRAYS, (2u << 24) | 3);   // 3-5: merged
    b.method(m::NV4097_DRAW_ARRAYS, (0u << 24) | 100); // a separate range
    b.method(m::NV4097_SET_BEGIN_END, 0);

    b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::TriangleStrip));
    b.method(m::NV4097_DRAW_INDEX_ARRAY, (255u << 24) | 16);
    b.method(m::NV4097_SET_BEGIN_END, 0);

    b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::Points));
    b.method_ni(m::NV4097_INLINE_ARRAY, {f2u(1.f), f2u(2.f), f2u(3.f)});
    b.method(m::NV4097_SET_BEGIN_END, 0);

    b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::Lines));
    b.method(m::NV4097_ARRAY_ELEMENT16, 0x00050004);
    b.method(m::NV4097_ARRAY_ELEMENT32, 9);
    b.method(m::NV4097_SET_BEGIN_END, 0);

    b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::Quads));
    for (u32 i = 0; i < 4; i++) {
        b.method(m::NV4097_SET_VERTEX_DATA4UB_M + 3, 0xff0000ffu); // colour (attr 3) before the position
        b.method(m::NV4097_SET_VERTEX_DATA2F_M + 0, {f2u(static_cast<float>(i)), f2u(0.f)}); // attr 0: emits
    }
    b.method(m::NV4097_SET_BEGIN_END, 0);

    // Empty clause: no draw
    b.method(m::NV4097_SET_BEGIN_END, static_cast<u32>(Primitive::Triangles));
    b.method(m::NV4097_SET_BEGIN_END, 0);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.empty());

    const auto draws = f.backend.of(Kind::Draw);
    REQUIRE(draws.size() == 5);
    const DrawClause& d0 = draws[0]->draw;
    CHECK(d0.primitive == Primitive::Triangles);
    CHECK(d0.command == DrawCommand::Array);
    REQUIRE(d0.ranges.size() == 2);
    CHECK(d0.ranges[0].first == 0);
    CHECK(d0.ranges[0].count == 6);
    CHECK(d0.ranges[1].first == 100);
    CHECK(d0.ranges[1].count == 1);
    CHECK(d0.vertex_count() == 7);

    CHECK(draws[1]->draw.command == DrawCommand::Indexed);
    CHECK(draws[1]->draw.ranges[0].first == 16);
    CHECK(draws[1]->draw.ranges[0].count == 256);

    CHECK(draws[2]->draw.command == DrawCommand::InlineArray);
    CHECK(draws[2]->draw.inline_words == std::vector<u32>{f2u(1.f), f2u(2.f), f2u(3.f)});

    CHECK(draws[3]->draw.command == DrawCommand::None);
    CHECK(draws[3]->draw.array_elements == std::vector<u32>{4, 5, 9});

    const DrawClause& imm = draws[4]->draw;
    CHECK(imm.primitive == Primitive::Quads);
    REQUIRE(imm.immediate_vertex_count == 4);
    CHECK(imm.immediate[0][2] == float4{2.f, 0.f, 0.f, 1.f});
    CHECK(imm.immediate[3][1][0] == 1.f);
    CHECK(imm.immediate[3][1][1] == 0.f);
}

TEST_CASE("method names for logs")
{
    CHECK(method_name(m::NV4097_SET_BLEND_ENABLE) == "NV4097_SET_BLEND_ENABLE");
    CHECK(method_name(m::NV4097_SET_TEXTURE_FORMAT + 3 * 8) == "NV4097_SET_TEXTURE_FORMAT[3]");
    CHECK(method_name(m::NV308A_COLOR + 17) == "NV308A_COLOR[17]");
    CHECK(method_name(m::NV4097_SET_TRANSFORM_CONSTANT + 31) == "NV4097_SET_TRANSFORM_CONSTANT[31]");
    CHECK(method_name(m::NV406E_SEMAPHORE_ACQUIRE) == "NV406E_SEMAPHORE_ACQUIRE");
    CHECK(method_name(0x1234 >> 2) == "unknown");
    CHECK(method_label(m::NV4097_SET_BEGIN_END) == "NV4097_SET_BEGIN_END (0x1808)");
    CHECK(is_known_method(m::GCM_FLIP_COMMAND));
    CHECK(is_known_method(m::NV3089_IMAGE_IN));
    CHECK_FALSE(is_known_method(0x1234 >> 2));
    CHECK_FALSE(is_known_method(0x7000 >> 2));
}
