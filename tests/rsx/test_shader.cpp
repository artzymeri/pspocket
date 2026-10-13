// Shader microcode: decoding, disassembly, hashing (embedded constants masked) and MSL generation for the supported
// subset, with "unsupported" errors for the rest. Programs are assembled in code (shader_asm.hpp). The Metal tests
// compile the generated MSL.

#include <catch_amalgamated.hpp>

#include <format>

#include "pspocket/rsx/shader/ir.hpp"
#include "pspocket/rsx/shader/msl.hpp"
#include "rsx_test_util.hpp"
#include "shader_asm.hpp"

using namespace pspocket;
using namespace pspocket::rsx;
using namespace pspocket::rsx::shader;
using namespace pspocket::rsx::test;

namespace {

// o[0] = v[0]; R1 = v[3] * c[4] + v[0]; o[1] = R1; o[7].xy = rcp(v[8].x)  (R2 via the scalar unit)
std::vector<VpInst> sample_vp()
{
    VpInst mov_pos;
    mov_pos.vec_op = static_cast<u32>(VpVecOp::MOV);
    mov_pos.src[0] = vp_in();
    mov_pos.input = 0;
    mov_pos.out = 0;
    mov_pos.vec_mask = 0xf;

    VpInst mad;
    mad.vec_op = static_cast<u32>(VpVecOp::MAD);
    mad.src[0] = vp_in();
    mad.src[1] = vp_const();
    mad.src[2] = vp_temp(0);
    mad.input = 3;
    mad.constant = 4;
    mad.vec_result = false;
    mad.vec_temp = 1;
    mad.vec_mask = 0xf;

    VpInst mov_col;
    mov_col.vec_op = static_cast<u32>(VpVecOp::MOV);
    mov_col.src[0] = vp_temp(1);
    mov_col.out = 1;
    mov_col.vec_mask = 0xf;

    VpInst rcp;
    rcp.sca_op = static_cast<u32>(VpScaOp::RCP);
    rcp.src[2] = vp_in("xxxx");
    rcp.input = 8;
    rcp.vec_result = false; // the scalar op writes the output
    rcp.out = 7;
    rcp.sca_mask = mask("xy");
    rcp.end = true;
    return {mov_pos, mad, mov_col, rcp};
}

} // namespace

TEST_CASE("vertex program decode and disassembly")
{
    const std::vector<u32> words = assemble_vp(sample_vp());
    std::vector<u32> memory(kMaxVertexProgramInstructions * 4, 0);
    std::copy(words.begin(), words.end(), memory.begin() + 8 * 4); // program at slot 8
    std::string error;
    const auto vp = decode_vertex_program(memory.data(), kMaxVertexProgramInstructions, 8, &error);
    REQUIRE(vp.has_value());
    REQUIRE(vp->instructions.size() == 4);
    CHECK(vp->start == 8);
    CHECK(vp->input_mask == ((1u << 0) | (1u << 3) | (1u << 8)));
    CHECK(vp->output_mask == ((1u << 0) | (1u << 1) | (1u << 7)));
    CHECK_FALSE(vp->has_branches);

    const VpInstruction& mad = vp->instructions[1];
    CHECK(mad.vec_op == static_cast<u8>(VpVecOp::MAD));
    CHECK(mad.src[0].type == VpRegType::Input);
    CHECK(mad.src[1].type == VpRegType::Constant);
    CHECK(mad.src[2].type == VpRegType::Temp);
    CHECK(mad.input == 3);
    CHECK(mad.constant == 4);
    CHECK(mad.vec_temp == 1);
    CHECK_FALSE(mad.writes_output());

    CHECK(disassemble(vp->instructions[0]) == "MOV o[0], v[0];");
    CHECK(disassemble(mad) == "MAD R1, v[3], c[4], R0;");
    CHECK(disassemble(vp->instructions[3]) == "RCP o[7].xy, v[8].xxxx; END");

    std::string bad;
    std::vector<u32> empty(kMaxVertexProgramInstructions * 4, 0);
    CHECK_FALSE(decode_vertex_program(empty.data(), kMaxVertexProgramInstructions, 0, &bad).has_value());
    CHECK(bad.find("no end") != std::string::npos);
}

TEST_CASE("a vertex program uploaded through the FIFO decodes from program memory")
{
    Fixture f;
    const std::vector<u32> words = assemble_vp(sample_vp());
    auto b = f.at(0);
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, 20);
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM, words);
    b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 20);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    const auto vp = decode_vertex_program(f.state().transform_program.data(), kMaxVertexProgramInstructions,
        f.state().transform_program_start());
    REQUIRE(vp.has_value());
    CHECK(vp->instructions.size() == 4);
    CHECK(vp->instructions[1].constant == 4);
}

TEST_CASE("vertex program hashes follow the code, not the location")
{
    const std::vector<u32> words = assemble_vp(sample_vp());
    std::vector<u32> memory(kMaxVertexProgramInstructions * 4, 0);
    std::copy(words.begin(), words.end(), memory.begin());
    std::copy(words.begin(), words.end(), memory.begin() + 100 * 4);
    const auto a = decode_vertex_program(memory.data(), kMaxVertexProgramInstructions, 0);
    const auto b = decode_vertex_program(memory.data(), kMaxVertexProgramInstructions, 100);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->hash == b->hash);
    memory[100 * 4 + 5] ^= 1u << 12; // change the MAD's constant index
    const auto c = decode_vertex_program(memory.data(), kMaxVertexProgramInstructions, 100);
    CHECK(c->hash != a->hash);
}

TEST_CASE("vertex program MSL for the supported subset")
{
    const std::vector<u32> words = assemble_vp(sample_vp());
    const auto vp = decode_vertex_program(words.data(), static_cast<u32>(words.size() / 4), 0);
    REQUIRE(vp.has_value());
    const TranslateResult r = translate_vertex_program(*vp);
    REQUIRE(r.ok);
    const std::string& s = r.shader.source;
    CHECK(s.find("vertex rsx_vs_out " + r.shader.entry) != std::string::npos);
    CHECK(s.find("const float4 v3 = rsx_fetch(p.attribs[3], vi, vb);") != std::string::npos);
    CHECK(s.find("fma(v3, vc[4], r0)") != std::string::npos);
    CHECK(s.find("float4 sca = float4(1.0 / (v8.xxxx).x);") != std::string::npos);
    CHECK(s.find("o7.xy = sca.xy;") != std::string::npos);
    CHECK(s.find("out.tc0 = o7;") != std::string::npos);
    CHECK(r.shader.input_mask == ((1u << 0) | (1u << 3) | (1u << 8)));
}

TEST_CASE("vertex program condition codes: an update to CC1 and conditional writes")
{
    // SLT CC1.xy, v[0], c[0]: neither an output nor a temp, so the result goes to CC1 (RPCS3 GetDST).
    VpInst set;
    set.vec_op = static_cast<u32>(VpVecOp::SLT);
    set.src[0] = vp_in();
    set.src[1] = vp_const();
    set.vec_result = false;
    set.vec_mask = mask("xy");
    set.cond_update = true;
    set.cond_reg = 1;
    // MOV o[0] (GT CC1.yyyy), v[0]: a conditional write of the whole output.
    VpInst mov;
    mov.vec_op = static_cast<u32>(VpVecOp::MOV);
    mov.src[0] = vp_in();
    mov.out = 0;
    mov.vec_mask = 0xf;
    mov.cond_test = true;
    mov.cond = 4; // gt
    mov.cond_reg = 1;
    mov.cond_sw = {1, 1, 1, 1};
    // MOV R2.xz (LE CC1), v[0]: masked; and RCP o[7].x with cond 0 (never) is dropped.
    VpInst le;
    le.vec_op = static_cast<u32>(VpVecOp::MOV);
    le.src[0] = vp_in();
    le.vec_result = false;
    le.vec_temp = 2;
    le.vec_mask = mask("xz");
    le.cond_test = true;
    le.cond = 3; // lt | eq
    le.cond_reg = 1;
    VpInst never;
    never.sca_op = static_cast<u32>(VpScaOp::RCP);
    never.vec_result = false;
    never.out = 7;
    never.sca_mask = mask("x");
    never.cond_test = true;
    never.cond = 0;
    never.end = true;
    const auto words = assemble_vp({set, mov, le, never});
    const auto vp = decode_vertex_program(words.data(), static_cast<u32>(words.size() / 4), 0);
    REQUIRE(vp.has_value());
    const TranslateResult r = translate_vertex_program(*vp);
    INFO(r.error);
    REQUIRE(r.ok);
    const std::string& s = r.shader.source;
    INFO(s);
    CHECK(s.find("float4 cc0 = float4(0.0), cc1 = float4(0.0);") != std::string::npos);
    CHECK(s.find("cc1.xy = vec.xy;") != std::string::npos);
    CHECK(s.find("o0 = select(o0, vec, (cc1.yyyy > float4(0.0)));") != std::string::npos);
    CHECK(s.find("r2.xz = select(r2.xz, vec.xz, (cc1 <= float4(0.0)).xz);") != std::string::npos);
    CHECK(s.find("o7.x =") == std::string::npos);
}

TEST_CASE("unsupported vertex program features are named")
{
    std::vector<VpInst> prog = sample_vp();
    prog[1].vec_op = 0x17; // no such vector op
    auto words = assemble_vp(prog);
    auto vp = decode_vertex_program(words.data(), static_cast<u32>(words.size() / 4), 0);
    REQUIRE(vp.has_value());
    TranslateResult r = translate_vertex_program(*vp);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("opcode VEC?") != std::string::npos);
    CHECK(r.error.find("instruction 1") != std::string::npos);

    // TXL samples a vertex texture (D-093): tex_num is src1's register field.
    prog = sample_vp();
    prog[1].vec_op = static_cast<u32>(VpVecOp::TXL);
    prog[1].src[1] = vp_temp(2);
    words = assemble_vp(prog);
    vp = decode_vertex_program(words.data(), static_cast<u32>(words.size() / 4), 0);
    REQUIRE(vp.has_value());
    r = translate_vertex_program(*vp);
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK(r.shader.source.find("vtex2.sample(vsmp2, ") != std::string::npos);
    CHECK(r.shader.source.find("texture2d<float> vtex2 [[texture(2)]], sampler vsmp2 [[sampler(2)]]") != std::string::npos);
    CHECK(r.shader.texture_mask == 0x4);

    // Still refused, loudly: PSH (the stack ops).
    prog = sample_vp();
    prog[3].sca_op = static_cast<u32>(VpScaOp::PSH);
    words = assemble_vp(prog);
    vp = decode_vertex_program(words.data(), static_cast<u32>(words.size() / 4), 0);
    REQUIRE(vp.has_value());
    r = translate_vertex_program(*vp);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("PSH") != std::string::npos);

    // BRB tests a bit of the transform branch bits (D-113; it was refused before).
    prog = sample_vp();
    prog[3].sca_op = static_cast<u32>(VpScaOp::BRB);
    prog[3].branch_index = 5;
    prog[3].branch_if_true = false;
    words = assemble_vp(prog);
    vp = decode_vertex_program(words.data(), static_cast<u32>(words.size() / 4), 0);
    REQUIRE(vp.has_value());
    CHECK(vp->has_branches);
    r = translate_vertex_program(*vp);
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK(r.shader.source.find("((p.branch_bits >> 5u) & 1u) == 0u") != std::string::npos);
}

TEST_CASE("vertex programs: ARL and relative constants, branches, calls and returns translate (RPCS3 semantics)")
{
    // 0: ARL A0.y, c[3]          1: MOV o[1], c[A0.y + 5]      2: BRI L4 (always)
    // 3: MOV o[1], c[0] (skipped) 4: CAL L6                     5: MOV o[0], v[0] END
    // 6: MOV o[7], c[1]           7: RET
    VpInst arl;
    arl.vec_op = static_cast<u32>(VpVecOp::ARL);
    arl.src[0] = vp_const();
    arl.constant = 3;
    arl.addr_reg = 0;
    arl.vec_mask = 0x2;
    VpInst rel;
    rel.vec_op = static_cast<u32>(VpVecOp::MOV);
    rel.src[0] = vp_const();
    rel.constant = 5;
    rel.constant_relative = true;
    rel.addr_swizzle = 1;
    rel.out = 1;
    rel.vec_mask = 0xf;
    VpInst bri;
    bri.sca_op = static_cast<u32>(VpScaOp::BRI);
    bri.branch_target = 4;
    VpInst skipped = rel;
    skipped.constant_relative = false;
    skipped.constant = 0;
    VpInst cal;
    cal.sca_op = static_cast<u32>(VpScaOp::CAL);
    cal.branch_target = 6;
    VpInst pos;
    pos.vec_op = static_cast<u32>(VpVecOp::MOV);
    pos.out = 0;
    pos.vec_mask = 0xf;
    pos.end = true;
    VpInst sub = skipped;
    sub.constant = 1;
    sub.out = 7;
    VpInst ret;
    ret.sca_op = static_cast<u32>(VpScaOp::RET);
    ret.end = true;
    const auto words = assemble_vp({arl, rel, bri, skipped, cal, pos, sub, ret});
    const auto vp = decode_vertex_program(words.data(), static_cast<u32>(words.size() / 4), 0);
    REQUIRE(vp.has_value());
    CHECK(vp->instructions.size() == 8); // the subroutine after the main END is decoded too
    const TranslateResult r = translate_vertex_program(*vp);
    INFO(r.error);
    REQUIRE(r.ok);
    const std::string& s = r.shader.source;
    INFO(s);
    CHECK(s.find("a0 = int4(floor(vc[3]));") != std::string::npos);
    CHECK(s.find("vc[clamp(5 + a0.y, 0, 467)]") != std::string::npos);
    CHECK(s.find("switch (jump)") != std::string::npos);
    CHECK(s.find("if (true) { jump = 4; continue; }") != std::string::npos);
    CHECK(s.find("calls[depth++] = 5; jump = 6; continue;") != std::string::npos);
    CHECK(s.find("jump = depth > 0 ? calls[--depth] : -1; continue;") != std::string::npos);
}

namespace {

// R0 = f[COL0] * {0.5, 0.25, 1, 1} ; H2 = TEX0(f[TEX0]) ; R0 = R0 + {0.1, ...} ; END
std::vector<FpInst> sample_fp(float k0 = 0.5f)
{
    FpInst mul;
    mul.op = static_cast<u32>(FpOp::MUL);
    mul.dst = 0;
    mul.input = 1; // COL0
    mul.src[0] = fp_in();
    mul.src[1] = fp_const();
    mul.constant = std::array<float, 4>{k0, 0.25f, 1.f, 1.f};

    FpInst tex;
    tex.op = static_cast<u32>(FpOp::TEX);
    tex.dst = 2;
    tex.fp16 = true;
    tex.input = 4; // TEX0
    tex.tex = 0;
    tex.src[0] = fp_in();

    FpInst add;
    add.op = static_cast<u32>(FpOp::ADD);
    add.dst = 0;
    add.mask = mask("xyz");
    add.src[0] = fp_r(0);
    add.src[1] = fp_const("xxxx");
    add.constant = std::array<float, 4>{0.125f, 0.f, 0.f, 0.f};
    add.end = true;
    return {mul, tex, add};
}

} // namespace

TEST_CASE("fragment program decode, constants and disassembly")
{
    const std::vector<u8> bytes = assemble_fp(sample_fp());
    REQUIRE(bytes.size() == 32 + 16 + 32);
    std::string error;
    const auto fp = decode_fragment_program(bytes.data(), static_cast<u32>(bytes.size()), &error);
    REQUIRE(fp.has_value());
    REQUIRE(fp->instructions.size() == 3);
    CHECK(fp->ucode_length == 80);
    CHECK(fp->constant_offsets == std::vector<u32>{16, 64});
    CHECK(fp->texture_mask == 1);
    CHECK(fp->input_mask == ((1u << 1) | (1u << 4)));
    const FpInstruction& mul = fp->instructions[0];
    CHECK(mul.op == static_cast<u8>(FpOp::MUL));
    CHECK(mul.has_constant);
    float k;
    __builtin_memcpy(&k, &mul.constant[0], 4);
    CHECK(k == 0.5f);
    CHECK(fp->instructions[1].dst_fp16);
    CHECK(fp->instructions[1].offset == 32);
    CHECK(fp->instructions[2].mask == mask("xyz"));
    CHECK(disassemble(fp->instructions[0]) == "MULR R0, f[COL0], {0.5, 0.25, 1, 1};");
    CHECK(disassemble(fp->instructions[1]) == "TEXH H2, f[TEX0], TEX0;");
    CHECK(disassemble(fp->instructions[2]) == "ADDR R0.xyz, R0, {0.125, 0, 0, 0}.xxxx; END");

    std::vector<std::array<float, 4>> constants;
    read_fragment_constants(*fp, bytes.data(), constants);
    REQUIRE(constants.size() == 2);
    CHECK(constants[0] == std::array<float, 4>{0.5f, 0.25f, 1.f, 1.f});
    CHECK(constants[1][0] == 0.125f);

    // Cut before the instruction with the end bit: no program.
    CHECK_FALSE(decode_fragment_program(bytes.data(), 48, &error).has_value());
    CHECK(error.find("no end") != std::string::npos);
}

TEST_CASE("fragment program hashes mask out embedded constants")
{
    const auto a_bytes = assemble_fp(sample_fp(0.5f));
    const auto b_bytes = assemble_fp(sample_fp(0.75f)); // only a constant differs
    const auto a = decode_fragment_program(a_bytes.data(), static_cast<u32>(a_bytes.size()));
    const auto b = decode_fragment_program(b_bytes.data(), static_cast<u32>(b_bytes.size()));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->hash == b->hash);

    auto prog = sample_fp();
    prog[2].mask = mask("xy");
    const auto c_bytes = assemble_fp(prog);
    const auto c = decode_fragment_program(c_bytes.data(), static_cast<u32>(c_bytes.size()));
    REQUIRE(c.has_value());
    CHECK(c->hash != a->hash);
}

TEST_CASE("fragment program MSL for the supported subset")
{
    const auto bytes = assemble_fp(sample_fp());
    const auto fp = decode_fragment_program(bytes.data(), static_cast<u32>(bytes.size()));
    REQUIRE(fp.has_value());
    FpOptions opt;
    opt.shader_control = 0x40; // 32-bit exports: R0 is the colour
    const TranslateResult r = translate_fragment_program(*fp, opt);
    REQUIRE(r.ok);
    const std::string& s = r.shader.source;
    CHECK(s.find("fragment rsx_fs_out " + r.shader.entry) != std::string::npos);
    CHECK(s.find("(clamp(in.col0, 0.0, 1.0)) * (fc[0])") != std::string::npos);
    CHECK(s.find("t0.sample(s0, ((in.tc0).xy * fp.tex_scale[0].xy), bias(fp.tex_bias[0]))") != std::string::npos);
    CHECK(s.find("(r0) + (fc[1].xxxx)") != std::string::npos);
    CHECK(s.find("r0.xyz = res.xyz;") != std::string::npos);
    CHECK(s.find("out.c0 = r0;") != std::string::npos);
    CHECK(s.find("texture2d<float> t0 [[texture(0)]]") != std::string::npos);
    CHECK(r.shader.texture_mask == 1);
    CHECK(r.shader.constant_count == 2);

    // 16-bit exports read H0, but this program only wrote R0: R0 is exported.
    FpOptions half;
    const TranslateResult h = translate_fragment_program(*fp, half);
    REQUIRE(h.ok);
    CHECK(h.shader.source.find("out.c0 = r0;") != std::string::npos);

    // A shadow map on unit 0: the depth comparison against the coordinate's z, in every channel.
    FpOptions shadow = opt;
    shadow.texture_kind[0] = TextureKind::Shadow2D;
    const TranslateResult sh = translate_fragment_program(*fp, shadow);
    REQUIRE(sh.ok);
    CHECK(sh.shader.source.find("rsx_shadow(t0, s0, (in.tc0).xy * fp.tex_scale[0].xy, (in.tc0).z, fp.tex_scale[0])") !=
          std::string::npos);
    CHECK(sh.shader.source.find("texture2d<float> t0 [[texture(0)]]") != std::string::npos);
}

// Condition codes as RPCS3 (FragmentProgramDecompiler SetDst, AddCodeCond, AddFlowOp, GetRawCond): GTA V's frontend
// shaders set RC0 from a constant and test it (run 039: every draw with them was skipped before).
TEST_CASE("fragment program condition codes: set, conditional writes and a conditional KIL")
{
    // MOVR RC0, {-1, 0, 1, 2}  (no destination: the result goes to RC0)
    FpInst set;
    set.op = static_cast<u32>(FpOp::MOV);
    set.no_dest = true;
    set.set_cond = true;
    set.src[0] = fp_const();
    set.constant = std::array<float, 4>{-1.f, 0.f, 1.f, 2.f};
    // MOVR R0.xy (GT RC0.zwxx), f[COL0]  (a conditional write of two components)
    FpInst mov;
    mov.op = static_cast<u32>(FpOp::MOV);
    mov.dst = 0;
    mov.mask = mask("xy");
    mov.input = 1;
    mov.src[0] = fp_in();
    mov.exec_lt = mov.exec_eq = false;
    mov.cond_sw = swz("zwxx");
    // ADDR R1, R0, R0 and RC1 = R1  (set_cond with a destination, writing RC1)
    FpInst add;
    add.op = static_cast<u32>(FpOp::ADD);
    add.dst = 1;
    add.src[0] = fp_r(0);
    add.src[1] = fp_r(0);
    add.set_cond = true;
    add.cond_mod_reg = 1;
    // KIL (LT RC1.x)
    FpInst kil;
    kil.op = static_cast<u32>(FpOp::KIL);
    kil.exec_eq = kil.exec_gt = false;
    kil.cond_reg = 1;
    kil.cond_sw = swz("xxxx");
    FpInst out;
    out.op = static_cast<u32>(FpOp::MOV);
    out.src[0] = fp_r(0);
    out.end = true;
    const auto bytes = assemble_fp({set, mov, add, kil, out});
    const auto fp = decode_fragment_program(bytes.data(), static_cast<u32>(bytes.size()));
    REQUIRE(fp.has_value());
    CHECK(disassemble(fp->instructions[0]) == "MOVR RC0, {-1, 0, 1, 2};");
    FpOptions opt;
    opt.shader_control = 0x40;
    const TranslateResult r = translate_fragment_program(*fp, opt);
    INFO(r.error);
    REQUIRE(r.ok);
    const std::string& s = r.shader.source;
    INFO(s);
    CHECK(s.find("float4 cc0 = float4(0.0), cc1 = float4(0.0);") != std::string::npos);
    CHECK(s.find("cc0 = res;") != std::string::npos);
    CHECK(s.find("r0.xy = select(r0.xy, res.xy, (cc0.zwxx > float4(0.0)).xy);") != std::string::npos);
    CHECK(s.find("r1 = res;\n        cc1 = r1;") != std::string::npos);
    CHECK(s.find("if (any((cc1.xxxx < float4(0.0)))) discard_fragment();") != std::string::npos);

    // Every comparison the condition bits can ask for.
    struct Row {
        bool lt, eq, gt;
        const char* op;
    };
    for (const Row& row : {Row{false, true, true, ">="}, Row{true, true, false, "<="}, Row{true, false, true, "!="},
             Row{false, false, true, ">"}, Row{true, false, false, "<"}, Row{false, true, false, "=="}}) {
        FpInst k = kil;
        k.exec_lt = row.lt;
        k.exec_eq = row.eq;
        k.exec_gt = row.gt;
        const auto b = assemble_fp({set, k, out});
        const auto f = decode_fragment_program(b.data(), static_cast<u32>(b.size()));
        REQUIRE(f.has_value());
        const TranslateResult t = translate_fragment_program(*f, opt);
        REQUIRE(t.ok);
        CHECK(t.shader.source.find(std::format("(cc1.xxxx {} float4(0.0))", row.op)) != std::string::npos);
    }
    // A condition no component can meet: the instruction is left out.
    FpInst never = mov;
    never.exec_gt = false;
    const auto nb = assemble_fp({set, never, out});
    const auto nf = decode_fragment_program(nb.data(), static_cast<u32>(nb.size()));
    REQUIRE(nf.has_value());
    const TranslateResult n = translate_fragment_program(*nf, opt);
    REQUIRE(n.ok);
    CHECK(n.shader.source.find("select(") == std::string::npos);
}

TEST_CASE("fragment program DIVSQ, PK16, UP16 and RET")
{
    // R1 = DIVSQ(f[COL0], c), R2 = PK16(R1), R3 = UP16(R2), RET (GT RC0), R0 = R3
    FpInst divsq;
    divsq.op = static_cast<u32>(FpOp::DIVSQ);
    divsq.dst = 1;
    divsq.input = 1;
    divsq.src[0] = fp_in();
    divsq.src[1] = fp_const();
    divsq.constant = std::array<float, 4>{4.f, 0.f, 0.f, 0.f};
    FpInst pk;
    pk.op = static_cast<u32>(FpOp::PK16);
    pk.dst = 2;
    pk.src[0] = fp_r(1);
    FpInst up;
    up.op = static_cast<u32>(FpOp::UP16);
    up.dst = 3;
    up.src[0] = fp_r(2);
    FpInst ret;
    ret.op = static_cast<u32>(FpOp::RET);
    ret.exec_lt = ret.exec_eq = false;
    FpInst out;
    out.op = static_cast<u32>(FpOp::MOV);
    out.src[0] = fp_r(3);
    out.end = true;
    const auto bytes = assemble_fp({divsq, pk, up, ret, out});
    const auto fp = decode_fragment_program(bytes.data(), static_cast<u32>(bytes.size()));
    REQUIRE(fp.has_value());
    const TranslateResult r = translate_fragment_program(*fp);
    INFO(r.error);
    REQUIRE(r.ok);
    const std::string& s = r.shader.source;
    INFO(s);
    CHECK(s.find("rsx_divsq(") != std::string::npos);
    CHECK(s.find("select(a, a / sqrt(abs(b)), abs(a) > 0.0)") != std::string::npos); // a zero numerator stays zero
    CHECK(s.find("rsx_pack_unorm2x16(") != std::string::npos); // packed by hand (D-942)
    CHECK(s.find("= pack_float_to_") == std::string::npos);
    CHECK(s.find("(pack_float_to_") == std::string::npos);
    CHECK(s.find("unpack_unorm2x16_to_float(as_type<uint>(") != std::string::npos);
    CHECK(s.find("do {") != std::string::npos);
    CHECK(s.find("if (any((cc0 > float4(0.0)))) { fp_ret = true; break; }") != std::string::npos);
    CHECK(s.find("} while (false);") < s.find("out.c0 = "));
}

TEST_CASE("unsupported fragment program features are named")
{
    auto prog = sample_fp();
    prog[1].op = static_cast<u32>(FpOp::POW); // RPCS3's decompiler has no POW either
    auto bytes = assemble_fp(prog);
    auto fp = decode_fragment_program(bytes.data(), static_cast<u32>(bytes.size()));
    REQUIRE(fp.has_value());
    TranslateResult r = translate_fragment_program(*fp);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("opcode POW") != std::string::npos);
    CHECK(r.error.find("0x0020") != std::string::npos);

    prog = sample_fp();
    prog[1].op = static_cast<u32>(FpOp::CAL);
    bytes = assemble_fp(prog);
    fp = decode_fragment_program(bytes.data(), static_cast<u32>(bytes.size()));
    REQUIRE(fp.has_value());
    CHECK(fp->has_branches);
    r = translate_fragment_program(*fp);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("CAL") != std::string::npos);
}

TEST_CASE("fragment programs: IFE with else, LOOP with BRK and RET, and byte packing translate (RPCS3 semantics)")
{
    // 0x00: MOV R1, f[COL0] (sets RC0)    0x10: IFE (RC0 > 0) else 0x40, end 0x60
    // 0x20: MOV R0, {1, 0, 0, 1}           0x40: MOV R0, {0, 1, 0, 1}  (each with its 16-byte constant)
    // 0x60: LOOP 4 times, end 0xA0          0x70: UPB R2, R1    0x80: BRK (RC0.y < 0)    0x90: RET (RC0 == 0)
    // 0xA0: MOV R0, R2 END
    FpInst setcc;
    setcc.op = static_cast<u32>(FpOp::MOV);
    setcc.dst = 1;
    setcc.input = 1;
    setcc.src[0] = fp_in();
    setcc.set_cond = true;
    FpInst ife;
    ife.op = static_cast<u32>(FpOp::IFE);
    ife.exec_lt = false, ife.exec_eq = false, ife.exec_gt = true;
    fp_if(ife, 0x40, 0x60);
    FpInst then_mov;
    then_mov.op = static_cast<u32>(FpOp::MOV);
    then_mov.src[0] = fp_const();
    then_mov.constant = std::array<float, 4>{1.f, 0.f, 0.f, 1.f};
    FpInst else_mov = then_mov; // at 0x40
    else_mov.constant = std::array<float, 4>{0.f, 1.f, 0.f, 1.f};
    FpInst loop; // at 0x60: 4 times, the index from 0 by 1, up to 0xA0
    loop.op = static_cast<u32>(FpOp::LOOP);
    fp_loop(loop, 4, 0, 1, 0xA0);
    FpInst upb; // 0x70: UPB R2, R1
    upb.op = static_cast<u32>(FpOp::UPB);
    upb.dst = 2;
    upb.src[0] = fp_r(1);
    FpInst brk; // 0x80: BRK (RC0.y < 0)
    brk.op = static_cast<u32>(FpOp::BRK);
    brk.exec_lt = true, brk.exec_eq = false, brk.exec_gt = false;
    brk.cond_sw = {1, 1, 1, 1};
    FpInst ret; // 0x90: RET (EQ)
    ret.op = static_cast<u32>(FpOp::RET);
    ret.exec_lt = false, ret.exec_eq = true, ret.exec_gt = false;
    FpInst last; // 0xA0: MOV R0, R2 END
    last.op = static_cast<u32>(FpOp::MOV);
    last.src[0] = fp_r(2);
    last.end = true;
    const auto bytes = assemble_fp({setcc, ife, then_mov, else_mov, loop, upb, brk, ret, last});
    const auto fp = decode_fragment_program(bytes.data(), static_cast<u32>(bytes.size()));
    REQUIRE(fp.has_value());
    const TranslateResult r = translate_fragment_program(*fp);
    INFO(r.error);
    REQUIRE(r.ok);
    const std::string& s = r.shader.source;
    INFO(s);
    CHECK(s.find("if (any((cc0 > float4(0.0)))) {") != std::string::npos);
    CHECK(s.find("} else {") != std::string::npos);
    CHECK(s.find("if (true) for (int i0 = 0; i0 < 4; i0++) {") != std::string::npos);
    CHECK(s.find("const int a0 = 0 + i0 * (1);") != std::string::npos);
    CHECK(s.find("unpack_unorm4x8_to_float(as_type<uint>((r1).x))") != std::string::npos);
    CHECK(s.find("if (any((cc0.yyyy < float4(0.0)))) break;") != std::string::npos);
    CHECK(s.find("{ fp_ret = true; break; }") != std::string::npos);
    CHECK(s.find("if (fp_ret) break;") != std::string::npos);
    CHECK(s.find("bool fp_ret = false;") != std::string::npos);
}

namespace {

TranslateResult translate_fp(const std::vector<FpInst>& prog, const FpOptions& opt = {})
{
    const auto bytes = assemble_fp(prog);
    const auto fp = decode_fragment_program(bytes.data(), static_cast<u32>(bytes.size()));
    REQUIRE(fp.has_value());
    return translate_fragment_program(*fp, opt);
}

// OP R0, R1, R2, R3 (with the end bit)
FpInst fp_op3(FpOp op)
{
    FpInst in;
    in.op = static_cast<u32>(op);
    in.dst = 0;
    in.src[0] = fp_r(1);
    in.src[1] = fp_r(2);
    in.src[2] = fp_r(3);
    in.end = true;
    return in;
}

} // namespace

TEST_CASE("fragment programs: LIT, LIF, DST, REFL, DP2A, PKG, UPG, BEM, TEXBEM and TXPBEM translate (RPCS3, D-174)")
{
    struct Row {
        FpOp op;
        const char* code;
    };
    for (const Row& row : {
             Row{FpOp::LIT, "const float4 res = rsx_lit(r1);"},
             Row{FpOp::LIF, "const float4 res = rsx_lif(r1);"},
             Row{FpOp::DST, "float4(1.0, (r1).y * (r2).y, (r1).z, (r2).w)"},
             Row{FpOp::REFL, "const float4 res = rsx_refl(r1, r2);"},
             Row{FpOp::DP2A, "float4(dot((r1).xy, (r2).xy) + (r3).x)"},
             Row{FpOp::PKG, "float4(as_type<float>(rsx_pack_unorm4x8(rsx_linear_to_srgb(r1))))"},
             Row{FpOp::UPG, "rsx_srgb_to_linear(unpack_unorm4x8_to_float(as_type<uint>((r1).x)))"},
             Row{FpOp::BEM, "((r1).xyxy + (r2).xxxx * (r3).xzxz + (r2).yyyy * (r3).ywyw)"},
             Row{FpOp::TEXBEM, "const float4 x2d = ((r1).xyxy + (r2).xxxx * (r3).xzxz + (r2).yyyy * (r3).ywyw);"},
             Row{FpOp::TXPBEM, "const float4 x2d = ((r1).xyxy + (r2).xxxx * (r3).xzxz + (r2).yyyy * (r3).ywyw);"},
         }) {
        INFO(fp_op_name(static_cast<u32>(row.op)));
        const TranslateResult r = translate_fp({fp_op3(row.op)});
        INFO(r.error);
        REQUIRE(r.ok);
        INFO(r.shader.source);
        CHECK(r.shader.source.find(row.code) != std::string::npos);
    }
    // The helpers, as RPCS3 has them (RSXProgramCommon.glsl, RSXFragmentPrologue.glsl).
    const std::string lit = translate_fp({fp_op3(FpOp::LIT)}).shader.source;
    CHECK(lit.find("const float x = max(v.x, 0.0), y = max(v.y, 1e-10);") != std::string::npos);
    CHECK(lit.find("float4(1.0, x, x > 0.0 ? exp2(v.w * log2(y)) : 0.0, 1.0)") != std::string::npos);
    CHECK(lit.find("float4(1.0, v.y, v.y > 0.0 ? exp2(v.w) : 0.0, 1.0)") != std::string::npos);
    CHECK(translate_fp({fp_op3(FpOp::REFL)}).shader.source.find("return i - 2.0 * dot(n, i) * n;") != std::string::npos);
    CHECK(translate_fp({fp_op3(FpOp::PKG)}).shader.source.find("clamp(select(high, low, c < 0.0031308), 0.0, 1.0)") !=
          std::string::npos);
    CHECK(translate_fp({fp_op3(FpOp::UPG)}).shader.source.find("pow((c + 0.055) / 1.055, float4(2.4)), c > 0.04045)") !=
          std::string::npos);
    // TEXBEM samples x2d as TEX does its coordinate, TXPBEM as TXP.
    const std::string bem = translate_fp({fp_op3(FpOp::TEXBEM)}).shader.source;
    CHECK(bem.find("t0.sample(s0, ((x2d).xy * fp.tex_scale[0].xy), bias(fp.tex_bias[0]))") != std::string::npos);
    const std::string txpbem = translate_fp({fp_op3(FpOp::TXPBEM)}).shader.source;
    CHECK(txpbem.find("t0.sample(s0, (((x2d).xy / (x2d).w) * fp.tex_scale[0].xy), bias(fp.tex_bias[0]))") != std::string::npos);
    // LIT, LIF and REFL are not precision-clamped (RPCS3 SetDst); DST is.
    for (const FpOp op : {FpOp::LIT, FpOp::LIF, FpOp::REFL, FpOp::DST}) {
        FpInst in = fp_op3(op);
        in.prec = 2; // fixed12
        const std::string s = translate_fp({in}).shader.source;
        INFO(fp_op_name(static_cast<u32>(op)));
        CHECK((s.find("clamp(") != std::string::npos && s.find(", -2.0, 2.0)") != std::string::npos) == (op == FpOp::DST));
    }
    // Disassembly names the unit and the three sources.
    const auto bytes = assemble_fp({fp_op3(FpOp::TEXBEM)});
    const auto fp = decode_fragment_program(bytes.data(), static_cast<u32>(bytes.size()));
    REQUIRE(fp.has_value());
    CHECK(disassemble(fp->instructions[0]) == "TEXBEMR R0, R1, R2, R3, TEX0; END");
}

TEST_CASE("fragment program TXP on a cube map divides by w; TXD on a shadow map compares (RPCS3, D-174)")
{
    FpInst txp; // TXP R0, f[TEX0], TEX0 (a cube map)
    txp.op = static_cast<u32>(FpOp::TXP);
    txp.input = 4;
    txp.src[0] = fp_in();
    FpInst txd; // TXD R1, f[TEX1], R2, R3, TEX1 (a shadow map)
    txd.op = static_cast<u32>(FpOp::TXD);
    txd.dst = 1;
    txd.input = 5;
    txd.tex = 1;
    txd.src[0] = fp_in();
    txd.src[1] = fp_r(2);
    txd.src[2] = fp_r(3);
    txd.end = true;
    FpOptions opt;
    opt.texture_kind[0] = TextureKind::Cube;
    opt.texture_kind[1] = TextureKind::Shadow2D;
    const TranslateResult r = translate_fp({txp, txd}, opt);
    INFO(r.error);
    REQUIRE(r.ok);
    const std::string& s = r.shader.source;
    INFO(s);
    CHECK(s.find("t0.sample(s0, ((in.tc0).xyz / (in.tc0).w)") != std::string::npos);
    CHECK(s.find("rsx_shadow(t1, s1, (in.tc1).xy * fp.tex_scale[1].xy, (in.tc1).z, fp.tex_scale[1])") != std::string::npos);
    CHECK(s.find("texturecube<float> t0 [[texture(0)]]") != std::string::npos);
}

TEST_CASE("fragment program texel conversions, alpha kill and LOD bias, per unit (RPCS3 _process_texel, D-174)")
{
    // TEX R0, f[TEX0], TEX0    TXL R1, f[TEX1], TEX1    TXD_bx2 R2, f[TEX2], R3, R4, TEX2    TEX_bx2 R5, f[TEX3], TEX3 END
    const auto sample = [](FpOp op, u32 unit, bool bx2) {
        FpInst in;
        in.op = static_cast<u32>(op);
        in.dst = unit == 3 ? 5 : unit;
        in.input = 4 + unit;
        in.tex = unit;
        in.src[0] = fp_in();
        in.src[1] = fp_r(3);
        in.src[2] = fp_r(4);
        in.texture_expand = bx2;
        return in;
    };
    std::vector<FpInst> prog{sample(FpOp::TEX, 0, false), sample(FpOp::TXL, 1, false), sample(FpOp::TXD, 2, true),
        sample(FpOp::TEX, 3, true)};
    prog.back().end = true;
    FpOptions opt;
    opt.texture_convert_mask = 0x1;
    opt.texture_alpha_kill_mask = 0x1;
    opt.texture_kind[3] = TextureKind::Shadow2D;
    const TranslateResult r = translate_fp(prog, opt);
    INFO(r.error);
    REQUIRE(r.ok);
    const std::string& s = r.shader.source;
    INFO(s);
    // Unit 0: alpha kill on the texel as sampled, then its conversions; the unit's LOD bias on the implicit LOD.
    CHECK(s.find("float4 tex = t0.sample(s0, ((in.tc0).xy * fp.tex_scale[0].xy), bias(fp.tex_bias[0]));\n"
                 "        if (tex.w < 0.000001) discard_fragment(); // alpha kill\n"
                 "        tex = rsx_texel(tex, fp.tex_control[0], false);") != std::string::npos);
    // Unit 1: TXL's level plus the bias, nothing converted.
    CHECK(s.find("t1.sample(s1, ((in.tc1).xy * fp.tex_scale[1].xy), level((in.tc1).w + fp.tex_bias[1]))") != std::string::npos);
    CHECK(s.find("fp.tex_control[1]") == std::string::npos);
    // Unit 2: _bx2 converts as the unit's format allows; TXD's derivatives grow by 2^bias.
    CHECK(s.find("gradient2d((r3).xy * fp.tex_scale[2].xy * exp2(fp.tex_bias[2]), (r4).xy * fp.tex_scale[2].xy * "
                 "exp2(fp.tex_bias[2]))") != std::string::npos);
    CHECK(s.find("tex = rsx_texel(tex, fp.tex_control[2], true);") != std::string::npos);
    // Unit 3: a shadow lookup: neither _bx2 nor conversions (RPCS3 TEX2D_SHADOW).
    CHECK(s.find("fp.tex_control[3]") == std::string::npos);
    CHECK(s.find("const float4 res = rsx_shadow(t3, s3, (in.tc3).xy * fp.tex_scale[3].xy, (in.tc3).z, fp.tex_scale[3]);") !=
          std::string::npos);
    CHECK(s.find("static float4 rsx_texel(float4 v, uint c, bool bx2)") != std::string::npos);
    CHECK(s.find("uint tex_control[16];") != std::string::npos);
    // Without conversions, alpha kill or _bx2, no helper and no extra statements.
    FpInst only = sample(FpOp::TEX, 0, false);
    only.end = true;
    const TranslateResult plain = translate_fp({only}, FpOptions{});
    REQUIRE(plain.ok);
    CHECK(plain.shader.source.find("rsx_texel") == std::string::npos);
    CHECK(plain.shader.source.find("discard_fragment") == std::string::npos);
}

TEST_CASE("fragment program outputs: sRGB, then the alpha test and alpha to coverage on colour 0 (RPCS3's ROP, D-174)")
{
    FpInst mov; // MOV H0, f[COL0] END (16-bit exports: H0, H4, H6, H8)
    mov.op = static_cast<u32>(FpOp::MOV);
    mov.fp16 = true;
    mov.input = 1;
    mov.src[0] = fp_in();
    mov.end = true;
    FpOptions opt;
    opt.mrt_count = 2;
    opt.srgb_output = true;
    opt.alpha_test = true;
    opt.alpha_to_coverage = true;
    opt.output_swizzle[0] = {2, 2, 2, 2}; // a B8 target: blue in every channel
    TranslateResult r = translate_fp({mov}, opt);
    INFO(r.error);
    REQUIRE(r.ok);
    std::string s = r.shader.source;
    {
        INFO(s);
        const usize srgb = s.find("    h0.xyz = rsx_linear_to_srgb(h0).xyz;\n    h4.xyz = rsx_linear_to_srgb(h4).xyz;\n");
        const usize test = s.find("    if (!rsx_compare(fp.alpha_func, h0.w, fp.alpha_ref)) discard_fragment();\n");
        const usize a2c = s.find("    if (!(h0.w > fract(sin(dot(in.position.xy, float2(12.9898, 78.233))) * 43758.5453))) "
                                 "discard_fragment();");
        const usize out = s.find("    out.c0 = float4(h0.z, h0.z, h0.z, h0.z);");
        CHECK(srgb != std::string::npos);
        CHECK(test != std::string::npos);
        CHECK(a2c != std::string::npos);
        CHECK(out != std::string::npos);
        CHECK((srgb < test && test < a2c && a2c < out)); // RPCS3 RSXROPEpilogue.glsl's order
        CHECK(s.find("static float4 rsx_linear_to_srgb(float4 c)") != std::string::npos);
    }
    // Without colour outputs the alpha test still runs ("This stage always runs even without a color output").
    opt.mrt_count = 0;
    opt.srgb_output = opt.alpha_to_coverage = false;
    r = translate_fp({mov}, opt);
    REQUIRE(r.ok);
    s = r.shader.source;
    CHECK(s.find("if (!rsx_compare(fp.alpha_func, h0.w, fp.alpha_ref)) discard_fragment();") != std::string::npos);
    CHECK(s.find("rsx_linear_to_srgb") == std::string::npos);
}

TEST_CASE("fragment program loops run their count; the LOOP index picks texture coordinates (NV_fragment_program2, D-174)")
{
    // 0x00: LOOP 3 times, index from 1 by 2, end 0x20    0x10: ADD R0, R0, f[TEX(A0.x + 2)]    0x20: MOV R0, R0 END
    FpInst loop;
    loop.op = static_cast<u32>(FpOp::LOOP);
    fp_loop(loop, 3, 1, 2, 0x20);
    FpInst add;
    add.op = static_cast<u32>(FpOp::ADD);
    add.src[0] = fp_r(0);
    add.src[1] = fp_in();
    fp_indexed(add, 2);
    FpInst last;
    last.op = static_cast<u32>(FpOp::MOV);
    last.src[0] = fp_r(0);
    last.end = true;
    FpOptions opt;
    opt.texcoord_2d_mask = 0x2; // TEX1: (x, y, 0, w), in the table too
    TranslateResult r = translate_fp({loop, add, last}, opt);
    INFO(r.error);
    REQUIRE(r.ok);
    std::string s = r.shader.source;
    {
        INFO(s);
        CHECK(s.find("if (true) for (int i0 = 0; i0 < 3; i0++) {") != std::string::npos);
        CHECK(s.find("const int a0 = 1 + i0 * (2);") != std::string::npos);
        CHECK(s.find("(uint(a0 + 2) < 10u ? tcs[(a0 + 2)] : float4(0.0, 0.0, 0.0, 1.0))") != std::string::npos);
        CHECK(s.find("const float4 tcs[10] = {in.tc0, float4(in.tc1.xy, 0.0, in_w), in.tc2,") != std::string::npos);
        CHECK(s.find("float4 tc9 [[user(tc9)]];") != std::string::npos); // every coordinate an index can pick
    }
    const auto bytes = assemble_fp({loop, add, last});
    const auto fp = decode_fragment_program(bytes.data(), static_cast<u32>(bytes.size()));
    REQUIRE(fp.has_value());
    CHECK(disassemble(fp->instructions[1]) == "ADDR R0, R0, f[TEX(A0.x + 2)];");

    // A REP with every counter field the count, as NVIDIA's compilers encode it (nouveau nv40_fp_rep): 5 runs.
    FpInst rep = loop;
    rep.op = static_cast<u32>(FpOp::REP);
    fp_loop(rep, 5, 5, 5, 0x20);
    r = translate_fp({rep, add, last});
    REQUIRE(r.ok);
    s = r.shader.source;
    {
        INFO(s);
        CHECK(s.find("if (true) for (int i0 = 0; i0 < 5; i0++) {") != std::string::npos);
        CHECK(s.find("const int a0") == std::string::npos); // REP leaves the index...
        CHECK(s.find("tcs[(i0 + 2)]") != std::string::npos); // ...so with no LOOP around, its counter (RPCS3) indexes
    }
    // REP inside LOOP: the LOOP's index. 0x00 LOOP end 0x40, 0x10 REP end 0x30, 0x20 ADD, 0x30 NOP, 0x40 MOV END.
    FpInst outer = loop, inner = rep;
    fp_loop(outer, 2, 0, -1, 0x40);
    fp_loop(inner, 2, 0, 0, 0x30);
    r = translate_fp({outer, inner, add, FpInst{}, last});
    REQUIRE(r.ok);
    s = r.shader.source;
    {
        INFO(s);
        CHECK(s.find("const int a0 = 0 + i0 * (-1);") != std::string::npos); // a signed increment
        CHECK(s.find("for (int i1 = 0; i1 < 2; i1++)") != std::string::npos);
        CHECK(s.find("tcs[(a0 + 2)]") != std::string::npos);
    }
    // A condition that never holds skips the loop (RPCS3 "if (false) for (...)").
    FpInst never = loop;
    never.exec_lt = never.exec_eq = never.exec_gt = false;
    r = translate_fp({never, add, last});
    REQUIRE(r.ok);
    CHECK(r.shader.source.find("if (false) for (int i0 = 0; i0 < 3; i0++) {") != std::string::npos);
    // BRK outside a loop does nothing (RPCS3); a break there would leave the program.
    FpInst brk;
    brk.op = static_cast<u32>(FpOp::BRK);
    r = translate_fp({brk, last});
    REQUIRE(r.ok);
    const std::string brk_body = r.shader.source.substr(r.shader.source.find("    do {\n"));
    CHECK(brk_body.find("break;") == std::string::npos);
    CHECK(brk_body.find("// (outside a loop: no effect, as RPCS3)") != std::string::npos);
    // Outside loops A0.x is 0: the input is TEX(offset) itself.
    FpInst direct = add;
    direct.end = true;
    fp_indexed(direct, 3);
    r = translate_fp({direct});
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK(r.shader.source.find("(r0) + (in.tc3)") != std::string::npos);
    CHECK(r.shader.source.find("tcs[") == std::string::npos);
    // ... and past SSA it is no input at all (RPCS3: invalid ucode, nothing drawn): refused loudly.
    fp_indexed(direct, 11);
    r = translate_fp({direct});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("TEX0 + 11") != std::string::npos);
}

TEST_CASE("fragment program opcode names cover the ISA")
{
    CHECK(std::string(fp_op_name(0x17)) == "TEX");
    CHECK(std::string(fp_op_name(0x45)) == "RET");
    CHECK(std::string(fp_op_name(0x30)) == "OP?");
    CHECK(fp_op_is_flow_control(static_cast<u32>(FpOp::LOOP)));
    CHECK_FALSE(fp_op_is_flow_control(static_cast<u32>(FpOp::KIL)));
    CHECK(std::string(vp_vec_op_name(0x19)) == "TXL");
    CHECK(std::string(vp_sca_op_name(0x14)) == "POP");
}

TEST_CASE("the translation's fingerprint is stable and covers what the translator writes (D-272)")
{
    const u64 a = translation_fingerprint(), b = translation_fingerprint();
    CHECK(a == b);
    CHECK(a != 0);
    CHECK(a != (0xcbf29ce484222325ull ^ kTranslationVersion)); // the programs and the prologue went into it
}
