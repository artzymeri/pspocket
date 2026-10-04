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

TEST_CASE("unsupported vertex program features are named")
{
    std::vector<VpInst> prog = sample_vp();
    prog[1].vec_op = static_cast<u32>(VpVecOp::ARL);
    auto words = assemble_vp(prog);
    auto vp = decode_vertex_program(words.data(), static_cast<u32>(words.size() / 4), 0);
    REQUIRE(vp.has_value());
    TranslateResult r = translate_vertex_program(*vp);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("opcode ARL") != std::string::npos);
    CHECK(r.error.find("instruction 1") != std::string::npos);

    prog = sample_vp();
    prog[3].sca_op = static_cast<u32>(VpScaOp::BRA);
    words = assemble_vp(prog);
    vp = decode_vertex_program(words.data(), static_cast<u32>(words.size() / 4), 0);
    REQUIRE(vp.has_value());
    CHECK(vp->has_branches);
    r = translate_vertex_program(*vp);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("BRA") != std::string::npos);
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
    CHECK(s.find("t0.sample(s0, ((in.tc0).xy * fp.tex_scale[0].xy))") != std::string::npos);
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

TEST_CASE("unsupported fragment program features are named")
{
    auto prog = sample_fp();
    prog[1].op = static_cast<u32>(FpOp::PK4);
    auto bytes = assemble_fp(prog);
    auto fp = decode_fragment_program(bytes.data(), static_cast<u32>(bytes.size()));
    REQUIRE(fp.has_value());
    TranslateResult r = translate_fragment_program(*fp);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("opcode PK4") != std::string::npos);
    CHECK(r.error.find("0x0020") != std::string::npos);

    prog = sample_fp();
    prog[1].op = static_cast<u32>(FpOp::IFE);
    bytes = assemble_fp(prog);
    fp = decode_fragment_program(bytes.data(), static_cast<u32>(bytes.size()));
    REQUIRE(fp.has_value());
    CHECK(fp->has_branches);
    r = translate_fragment_program(*fp);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("IFE") != std::string::npos);
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
