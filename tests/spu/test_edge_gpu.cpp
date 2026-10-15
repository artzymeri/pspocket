// EDGE geometry on the GPU, the CPU side (D-988): the kernels' code (pspocket/core/edge_kernels.h, the same source the
// Metal backend compiles) on a synthetic mesh against a straightforward reference; its screen transform against the
// SPU native one (0x0ECF8 of GTA V's EDGE build, edge_dad4.c) bit for bit (the kernels' emulation of the SPU's
// arithmetic on rounding to nearest against the SPU's own, in its environment); its rounding against the SPU's on
// random operands; the culling rules;
// and the stages' recorders (pspocket/spu/edge_gpu.hpp) over a synthetic EDGE context in local store. No game data:
// every input is made here (the transform's shuffle constants are the ones edge_dad4.c checks for).

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

#include "../rsx/edge_synthetic.hpp"
#include "native.h"
#include "pspocket/core/be.h"
#include "pspocket/core/edge_kernels.h"
#include "pspocket/spu/context.h"
#include "pspocket/spu/edge_gpu.hpp"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/lifted.h"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
namespace et = pspocket::edge_test;
namespace eg = pspocket::spu::edge_gpu;

namespace {

float f32(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
uint32_t u32(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

struct Run {
    std::vector<uint8_t> out;    // the output vertices
    std::vector<ek_screen> scr;  // the screen-space vertices
    std::vector<uint16_t> kept;  // the kept indices
};

Run run(const std::vector<uint8_t>& blob)
{
    const auto& j = *reinterpret_cast<const ps_edge_job*>(blob.data());
    Run r;
    r.out.assign(j.num_vertexes * j.out_stride + 16, 0);
    const std::vector<float> pal = ek_palette(blob.data());
    for (uint32_t v = 0; v < j.num_vertexes; v++) r.scr.push_back(ek_vertex(blob.data(), &j, pal.data(), v, r.out.data() + v * j.out_stride));
    const uint8_t* idx = blob.data() + j.indices;
    for (uint32_t i = 0; i + 2 < j.num_indexes; i += 3) {
        const uint16_t a = static_cast<uint16_t>(ps_ld16(idx + 2 * i)), b = static_cast<uint16_t>(ps_ld16(idx + 2 * i + 2));
        const uint16_t c = static_cast<uint16_t>(ps_ld16(idx + 2 * i + 4));
        if (!ek_culled(&j, r.scr[a], r.scr[b], r.scr[c])) r.kept.insert(r.kept.end(), {a, b, c});
    }
    return r;
}

// The reference's skinned position: the weights' blend of the bones' matrices, in doubles.
std::array<double, 3> skinned(const et::Mesh& m, uint32_t v, const std::array<double, 3>& p, bool vector)
{
    std::array<double, 12> r{};
    std::array<uint8_t, 8> w = m.weights[v];
    if (m.skin == 4) w = {255, w[0], 0, 0, 0, 0, 0, 0};
    else if (w[1] == 0xFF) w = {255, 0, 0, 0, 0, 0, 0, 0};
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 12; i++) r[i] += w[2 * k] / 255.0 * static_cast<double>(m.matrices[w[2 * k + 1]][i]);
    std::array<double, 3> o{};
    for (int i = 0; i < 3; i++) o[i] = r[4 * i] * p[0] + r[4 * i + 1] * p[1] + r[4 * i + 2] * p[2] + (vector ? 0.0 : r[4 * i + 3]);
    return o;
}

} // namespace

TEST_CASE("edge gpu: the kernels decode, skin and compress as EDGE does (synthetic mesh)")
{
    for (const uint32_t skin : {0u, 1u, 4u}) {
        const et::Mesh m = et::grid(skin, 2);
        const std::vector<uint8_t> blob = et::make_blob(m);
        const auto& j = *reinterpret_cast<const ps_edge_job*>(blob.data());
        const Run r = run(blob);
        double worst_pos = 0, worst_vec = 0;
        for (uint32_t v = 0; v < j.num_vertexes; v++) {
            // positions: the fixed point's own values, exactly, skinned
            std::array<double, 3> p;
            for (int k = 0; k < 3; k++)
                p[k] = std::ldexp(static_cast<double>(std::lround(std::ldexp(static_cast<double>(m.pos[v][k]), 12))), -12);
            const std::array<double, 3> want = skin ? skinned(m, v, p, false) : p;
            const uint8_t* o = r.out.data() + v * 24;
            for (int k = 0; k < 3; k++) worst_pos = std::max(worst_pos, std::fabs(static_cast<double>(f32(ps_ld32(o + 4 * k))) - want[k]));
            // the normal: 10-bit components, the third from them, skinned, then X11Y11Z10N
            std::array<double, 3> n{static_cast<double>(m.nrm[v][0]), static_cast<double>(m.nrm[v][1]),
                static_cast<double>(m.nrm[v][2])};
            if (skin) n = skinned(m, v, n, true);
            const uint32_t x = ps_ld32(o + 12);
            const auto field = [](uint32_t bits, uint32_t width, uint32_t shift) { // u = trunc((f + 1) h), stored u ^ 2^(w-1)
                const uint32_t u = ((bits >> shift) & ((1u << width) - 1)) ^ (1u << (width - 1));
                const double h = static_cast<double>(1u << (width - 1)) - 0.5;
                return (static_cast<double>(u) + 0.5) / h - 1.0;
            };
            worst_vec = std::max({worst_vec, std::fabs(field(x, 11, 0) - n[0]), std::fabs(field(x, 11, 11) - n[1]),
                std::fabs(field(x, 10, 22) - n[2])});
            // the tangent's w: its sign
            const int16_t tw = static_cast<int16_t>(ps_ld16(o + 22));
            CHECK((tw > 0) == (m.tan[v][3] > 0));
        }
        INFO("skinning " << skin);
        CHECK(worst_pos < 1e-5);
        CHECK(worst_vec < 4e-3); // two quantizations: 10 bits in, 10 or 11 out
    }
}

TEST_CASE("edge gpu: the kernels' fm, fa, fs and fma are the SPU's, on random operands")
{
    // the SPU's own (lifted.h, in its environment) against the kernels' emulation (rounding to nearest)
    std::mt19937 rng(988);
    std::uniform_int_distribution<int> expo(-20, 20);
    std::uniform_real_distribution<float> mant(-2.0f, 2.0f);
    const auto any = [&] { return std::ldexp(mant(rng), expo(rng)); };
    constexpr int n = 200000;
    std::vector<std::array<float, 3>> in(n);
    for (auto& v : in) v = {any(), any(), any()};
    for (int i = 0; i < n; i += 97) in[i][1] = 0.0f; // a zero factor
    std::vector<std::array<uint32_t, 5>> spu(n);
    const uint64_t token = ps_spu_fpenv_enter();
    for (int i = 0; i < n; i++) {
        const ps_spu_vec a = psl_splat32(u32(in[i][0])), b = psl_splat32(u32(in[i][1])), c = psl_splat32(u32(in[i][2]));
        spu[i] = {psl_fm(a, b)[0], psl_fa(a, b)[0], psl_fs(a, b)[0], psl_fma(a, b, c)[0], psl_fnms(a, b, c)[0]};
    }
    ps_spu_fpenv_leave(token);
    int same[5] = {};
    for (int i = 0; i < n; i++) {
        const float a = in[i][0], b = in[i][1], c = in[i][2];
        same[0] += u32(ek_fm(a, b)) == spu[i][0];
        same[1] += u32(ek_fa(a, b)) == spu[i][1];
        same[2] += u32(ek_fs(a, b)) == spu[i][2];
        same[3] += u32(ek_fma(a, b, c)) == spu[i][3];
        same[4] += u32(ek_fmaf(-a, b, c)) == spu[i][4];
    }
    CHECK(same[0] == n);
    CHECK(same[1] == n);
    CHECK(same[2] == n);
    CHECK(same[3] >= n - n / 100000); // a fused product whose error sum rounds the wrong way: rare
    CHECK(same[4] >= n - n / 100000);
}

TEST_CASE("edge gpu: the culling rules")
{
    ps_edge_job j{};
    j.cull_mode = 2;
    j.flip = 0;
    j.nopix = 1;
    const uint32_t emulated[4] = {0, 0, 1, 0}; // keep (the facing to cull, in front, w > 0) and not outside: culled
    std::memcpy(j.culled, emulated, sizeof emulated);
    const auto v = [](float x, float y, uint32_t code = 0, bool front = true) {
        ek_screen s;
        s.x = x;
        s.y = y;
        s.code = code;
        s.packed = (static_cast<uint32_t>(static_cast<int32_t>(x)) << 16) | (static_cast<uint32_t>(static_cast<int32_t>(y)) & 0xFFFF);
        s.packed = (s.packed & 0x7FFFFFFFu) | (front ? 0x80000000u : 0);
        return s;
    };
    // area > 0: the facing the mode culls
    CHECK(ek_culled(&j, v(0, 0), v(10, 0), v(0, 10)));
    CHECK_FALSE(ek_culled(&j, v(0, 0), v(0, 10), v(10, 0)));
    // the other facing culled once the flip turns it
    j.flip = 0x80000000u;
    CHECK_FALSE(ek_culled(&j, v(0, 0), v(10, 0), v(0, 10)));
    CHECK(ek_culled(&j, v(0, 0), v(0, 10), v(10, 0)));
    j.flip = 0;
    // a vertex behind the near plane or at w <= 0: not culled by facing
    CHECK_FALSE(ek_culled(&j, v(0, 0, 0x80000000u), v(10, 0), v(0, 10)));
    CHECK_FALSE(ek_culled(&j, v(0, 0, 0, false), v(10, 0), v(0, 10)));
    // all three past one edge: the SPU's arithmetic keeps it (its NaN, D-988); visible either way
    CHECK_FALSE(ek_culled(&j, v(300, 0, 0x10000000u), v(310, 0, 0x10000000u), v(300, 10, 0x10000000u)));
    // no pixel: one integer x or y for all three, or one x + y or x - y
    CHECK(ek_culled(&j, v(5.2f, 0), v(5.7f, 20), v(5.9f, 9)));
    CHECK(ek_culled(&j, v(0, 7.1f), v(20, 7.3f), v(9, 7.8f)));
    CHECK(ek_culled(&j, v(1, 1), v(2, 0), v(0, 2)));
    j.nopix = 0;
    CHECK_FALSE(ek_culled(&j, v(1, 1), v(2, 0), v(0, 2)));
    j.cull_mode = 0;
    CHECK_FALSE(ek_culled(&j, v(0, 0), v(10, 0), v(0, 10)));
}

namespace {

// Local store laid out as GTA V's EDGE build has it at the culling (an EDGE context at 0x13300, the program at its
// lifted address): what the transform native and the recorders read.
struct EdgeLs {
    static constexpr uint32_t kCtx = 0x13300, kAttr = 0x3F1F0, kPositions = 0x20000, kScreen = 0x28000;
    static constexpr uint32_t kIndices = 0x13830;
    std::vector<uint8_t> ls = std::vector<uint8_t>(PS_SPU_LS_SIZE, 0);
    SPUContext ctx{};

    void st32(uint32_t a, uint32_t v) { ps_st32(ls.data() + a, v); }
    void stf(uint32_t a, float v) { st32(a, u32(v)); }

    EdgeLs()
    {
        // the shuffle constants 0x0ECF8 checks for (edge_dad4.c k_0ecf8), at the program's own addresses
        static const struct {
            uint32_t addr, w[4];
        } k[] = {
            {0x12710, {0x00000000u, 0x00000000u, 0x00000000u, 0x3F800000u}},
            {0x129B0, {0x00011213u, 0x04051617u, 0x08091A1Bu, 0x0C0D1E1Fu}},
            {0x129C0, {0x80808004u, 0x05060707u, 0x80808080u, 0x80808080u}},
            {0x12A40, {0x80800001u, 0x80800203u, 0x80801415u, 0x80801617u}},
            {0x12AA0, {0x00010203u, 0x10111213u, 0x04050607u, 0x14151617u}},
            {0x12AB0, {0x00010203u, 0x10111213u, 0x08090A0Bu, 0x18191A1Bu}},
            {0x12AD0, {0x00010203u, 0x04050607u, 0x10111213u, 0x80808080u}},
            {0x12B00, {0x00010203u, 0x04050607u, 0x13808080u, 0x0C0D0E0Fu}},
            {0x12BB0, {0x04050607u, 0x14151617u, 0x0C0D0E0Fu, 0x1C1D1E1Fu}},
            {0x12BE0, {0x08090A0Bu, 0x18191A1Bu, 0x08090A0Bu, 0x18191A1Bu}},
            {0x12C00, {0x08090A0Bu, 0x0C0D0E0Fu, 0x10111213u, 0x80808080u}},
            {0x12C10, {0x18191A1Bu, 0x1C1D1E1Fu, 0x00010203u, 0x04050607u}},
            {0x12C40, {0x0A0B0203u, 0x06071213u, 0x16171A1Bu, 0x80808080u}},
        };
        for (const auto& q : k)
            for (int w = 0; w < 4; w++) st32(q.addr + 4 * w, q.w[w]);
        ps_spu_context_init(&ctx, ls.data(), nullptr);
        ctx.reloc = 0;
    }

    // The view: local-to-world, view-projection, viewport, rectangle and depth range.
    void view()
    {
        const float m0[12] = {0.9f, -0.1f, 0.05f, 2.0f, 0.1f, 0.95f, 0.0f, -1.0f, 0.0f, 0.02f, 1.0f, 5.0f};
        const float m1[16] = {1.2f, 0, 0, 0.1f, 0, 1.6f, 0, -0.2f, 0, 0, 0.6f, 0.5f, 0.02f, 0.01f, 0.18f, 1.0f};
        for (int i = 0; i < 12; i++) stf(kCtx + 0xA0 + 4 * i, m0[i]);
        for (int i = 0; i < 16; i++) stf(kCtx + 0xE0 + 4 * i, m1[i]);
        st32(kCtx + 0xD0, 0x00000000u);      // x 0, y 0
        st32(kCtx + 0xD4, 0x050002D0u);      // width 1280, height 720
        stf(kCtx + 0xD8, 0.0f);
        stf(kCtx + 0xDC, 1.0f);
        const float sc[4] = {640, -360, 0.5f, 0}, of[4] = {640, 360, 0.5f, 0};
        for (int i = 0; i < 4; i++) {
            stf(kCtx + 0x120 + 4 * i, sc[i]);
            stf(kCtx + 0x130 + 4 * i, of[i]);
        }
        ls[kCtx + 0x13D] = 1; // +0.5 on x and y, the "no pixel" test
    }
};

} // namespace

TEST_CASE("edge gpu: the kernels' screen transform is the SPU native 0x0ECF8's, bit for bit")
{
    EdgeLs e;
    e.view();
    const uint32_t s = EdgeLs::kCtx;
    const uint32_t n = 37;
    e.ls[s] = 0x85;                        // the top bit; output table 5
    ps_st16(e.ls.data() + s + 8, n);       // vertices
    e.st32(s + 0x10 + 4 * 5, EdgeLs::kScreen);
    e.st32(s + 0x5C, EdgeLs::kPositions);
    e.ls[EdgeLs::kAttr + 183] = 0;         // not the projecting path
    for (uint32_t v = 0; v < n; v++) {
        const float t = static_cast<float>(v);
        e.stf(EdgeLs::kPositions + 16 * v, -3.0f + 0.17f * t);
        e.stf(EdgeLs::kPositions + 16 * v + 4, 2.0f - 0.11f * t);
        e.stf(EdgeLs::kPositions + 16 * v + 8, 0.5f * std::sin(t));
        e.stf(EdgeLs::kPositions + 16 * v + 12, 1.0f);
    }
    // the recorder's constants from the same context (the culling's entry: $3 the context, $4 the mode, $5 0)
    ps_st16(e.ls.data() + s + 0x17E, 3);
    e.st32(s + 0x50, EdgeLs::kIndices);
    SPUContext rc = e.ctx;
    rc.gpr[3].u32[3] = s;
    rc.gpr[4].u32[3] = 2;
    rc.gpr[5].u32[3] = 0;
    eg::Recording rec;
    rec.clear();
    rec.job.num_vertexes = n;
    rec.job.num_indexes = 3;
    const uint64_t token = ps_spu_fpenv_enter();
    REQUIRE(eg::on_cull(rec, e.ls.data(), rc));
    // the native, as the culling calls it
    SPUContext nc = e.ctx;
    nc.gpr[0].u32[3] = 0x10000;
    nc.gpr[1].u32[3] = 0x3E000;
    nc.gpr[3].u32[3] = s;
    nc.gpr[4].u32[3] = EdgeLs::kAttr;
    REQUIRE(ps_edge_dad4_0ecf8(&nc, e.ls.data()) == 1);
    ps_spu_fpenv_leave(token); // the kernels emulate the SPU's rounding on the default one
    uint32_t same = 0;
    for (uint32_t v = 0; v < n; v++) {
        float p[4];
        for (int k = 0; k < 4; k++) p[k] = f32(ps_ld32(e.ls.data() + EdgeLs::kPositions + 16 * v + 4 * k));
        const ek_screen g = ek_transform(&rec.job, p);
        const uint8_t* w = e.ls.data() + EdgeLs::kScreen + 16 * v;
        same += u32(g.x) == ps_ld32(w) && u32(g.y) == ps_ld32(w + 4) && g.code == (ps_ld32(w + 8) & 0xFC000000u) &&
                g.packed == ps_ld32(w + 12);
    }
    CHECK(same == n);
}

TEST_CASE("edge gpu: the recorders read an EDGE context and its streams as the work item has them")
{
    const et::Mesh m = et::grid(1, 2);
    const std::vector<uint8_t> want = et::make_blob(m);
    const auto& wj = *reinterpret_cast<const ps_edge_job*>(want.data());
    EdgeLs e;
    e.view();
    const uint32_t c = EdgeLs::kCtx, nv = wj.num_vertexes, ni = wj.num_indexes;
    // the configuration: 4 tables, hole, custom inputs, output format 2, no blend shapes, compressed CCW indices,
    // four-bone skinning, matrix format 0
    const uint8_t cfg[8] = {0x83, 0x1B, 0xFF, 0xFF, 0x02, 0xFF, 0x31, 0x00};
    std::memcpy(e.ls.data() + c, cfg, 8);
    ps_st16(e.ls.data() + c + 8, nv);
    ps_st16(e.ls.data() + c + 10, ni);
    for (uint32_t t = 0; t < 4; t++) e.st32(c + 0x10 + 4 * t, 0x30000 + 0x2000 * t); // the uniform tables
    e.st32(c + 0x174, 0x0ECF8);                                                     // the vertex-output callback
    e.st32(c + 0x178, EdgeLs::kAttr);
    // the streams and their descriptions in the in/out buffer
    const uint32_t v1 = 0x14000, fo1 = 0x15800, v2 = 0x16000, d1 = 0x15900, d2 = 0x15940;
    std::memcpy(e.ls.data() + v1, want.data() + wj.pos_data, nv * wj.pos_stride);
    std::memcpy(e.ls.data() + v2, want.data() + wj.uv_data, nv * wj.uv_stride);
    for (int k = 0; k < 3; k++) e.st32(fo1 + 4 * k, static_cast<uint32_t>(m.bias[k]));
    const uint8_t desc1[24] = {1, static_cast<uint8_t>(wj.pos_stride), 2, 0, 0, 0, 0, 0, 0, 8, 3, 1,
        static_cast<uint8_t>(wj.pos_stride), 0, 8, 0, 4, 12, 4, 12, 4, 12, 0, 0};
    const uint8_t desc2[24] = {2, 6, 2, 0, 0, 0, 0, 0, 0, 9, 3, 2, 3, 2, 0, 0, 3, 9, 4, 3, 3, 14, 0, 0};
    std::memcpy(e.ls.data() + d1, desc1, sizeof desc1);
    std::memcpy(e.ls.data() + d2, desc2, sizeof desc2);
    e.st32(c + 0x6C, d1);
    e.st32(c + 0x70, d2);
    eg::Recording r;
    r.clear();
    const uint64_t token = ps_spu_fpenv_enter();
    SPUContext dc = e.ctx;
    dc.gpr[0].u32[3] = 0x11000;
    dc.gpr[1].u32[3] = 0x3E000;
    dc.gpr[3].u32[3] = c;
    dc.gpr[4].u32[3] = v1;
    dc.gpr[5].u32[3] = fo1;
    dc.gpr[6].u32[3] = v2;
    dc.gpr[7].u32[3] = 0;
    // the job driver's own steps (D-990): a job with any of them on is left to the SPU (run 331's flickering shadows:
    // the triangle filter before the culling read tables the GPU path never made)
    {
        const uint32_t desc = 0x3A000; // descriptor + 0xF0, in $85
        SPUContext jc = dc;
        jc.gpr[85].u32[3] = desc;
        eg::Recording x;
        x.clear();
        CHECK(eg::on_decompress(x, e.ls.data(), jc));
        for (const uint32_t o : {0xA0u, 0xB6u, 0xB7u, 0xC0u, 0xDFu}) {
            e.ls[desc + o] = 1;
            x.clear();
            CHECK_FALSE(eg::on_decompress(x, e.ls.data(), jc));
            CHECK_FALSE(x.reject.empty());
            e.ls[desc + o] = 0;
        }
        e.ls[desc + 0xB4] = 0x04;
        x.clear();
        CHECK_FALSE(eg::on_decompress(x, e.ls.data(), jc));
        e.ls[desc + 0xB4] = 0;
        jc.gpr[83].u32[1] = 0x3F800000u; // a position offset
        x.clear();
        CHECK_FALSE(eg::on_decompress(x, e.ls.data(), jc));
        jc.gpr[83].u32[1] = 0x80000000u; // -0: none
        x.clear();
        CHECK(eg::on_decompress(x, e.ls.data(), jc));
    }
    REQUIRE(eg::on_decompress(r, e.ls.data(), dc));
    eg::leave_decompress(r, e.ls.data(), dc);
    CHECK(ps_ld32(e.ls.data() + c + 0x54) == v1);
    CHECK(ps_ld32(e.ls.data() + c + 0x58) == v1);
    CHECK(ps_ld32(e.ls.data() + c + 0x5C) == 0x30000);
    CHECK(ps_ld32(e.ls.data() + c + 0x60) == 0x32000);
    CHECK(ps_ld32(e.ls.data() + c + 0x64) == 0x34000);
    CHECK(ps_ld32(e.ls.data() + c + 0x150) == 0x01020300u);
    CHECK(dc.pc == 0x11000);
    // skinning: the palette and the bones
    const uint32_t mats = 0x13C00, weights = 0x13F00;
    std::memcpy(e.ls.data() + mats, want.data() + wj.matrices, 48 * wj.matrix_count);
    std::memcpy(e.ls.data() + weights, want.data() + wj.weights, 8 * nv);
    SPUContext sc = dc;
    sc.gpr[4].u32[3] = mats;
    sc.gpr[5].u32[3] = wj.matrix_count;
    sc.gpr[6].u32[3] = weights;
    REQUIRE(eg::on_skin(r, e.ls.data(), sc));
    eg::leave_skin(r, e.ls.data(), sc);
    CHECK(ps_ld32(e.ls.data() + c + 0x58) == mats);
    // culling: the decompressed indices
    std::memcpy(e.ls.data() + EdgeLs::kIndices, want.data() + wj.indices, 2 * ni);
    e.st32(c + 0x50, EdgeLs::kIndices);
    ps_st16(e.ls.data() + c + 0x17E, ni);
    SPUContext cc = sc;
    cc.gpr[4].u32[3] = 2;
    cc.gpr[5].u32[3] = 0;
    REQUIRE(eg::on_cull(r, e.ls.data(), cc));
    eg::leave_cull(r, e.ls.data(), cc);
    CHECK(cc.gpr[3].u32[3] == ni); // every index kept: the GPU culls
    CHECK(ps_ld32(e.ls.data() + c + 0x58) == ((EdgeLs::kIndices + 2 * ni + 15) & ~15u));
    {
        // told one triangle (the stand-in's way, D-990: the ring allocation no bigger than the vertices' need; told
        // every index, run 331's city filled the rings)
        eg::Recording t = r;
        t.told = 3;
        std::vector<uint8_t> ls = e.ls;
        SPUContext tc = sc;
        tc.gpr[4].u32[3] = 2;
        tc.gpr[5].u32[3] = 0;
        eg::leave_cull(t, ls.data(), tc);
        CHECK(tc.gpr[3].u32[3] == 3);
        CHECK(ps_ld16(ls.data() + c + 0x17E) == 3);
        CHECK(ps_ld32(ls.data() + c + 0x58) == ((EdgeLs::kIndices + 6 + 15) & ~15u));
    }
    // compression: the output vertex after the indices
    SPUContext oc = cc;
    oc.gpr[3].u32[3] = c;
    REQUIRE(eg::on_compress(r, e.ls.data(), oc));
    eg::leave_compress(r, e.ls.data(), oc);
    const uint32_t start = (EdgeLs::kIndices + 2 * ni + 15) & ~15u;
    CHECK(oc.gpr[3].u32[3] == start);
    CHECK(ps_ld32(e.ls.data() + c + 0x58) == ((start + 24 * nv + 15) & ~15u));
    ps_spu_fpenv_leave(token);
    r.finish();
    const auto& j = *reinterpret_cast<const ps_edge_job*>(r.blob.data());
    CHECK(j.num_vertexes == nv);
    CHECK(j.num_indexes == ni);
    CHECK(j.pos_stride == wj.pos_stride);
    CHECK(j.skin == 1);
    CHECK(j.out_stride == 24);
    CHECK(j.out_count == 3);
    CHECK(j.cull_mode == 2);
    // the same vertices as the work item made directly (the view aside: compare the outputs)
    const Run a = run(r.blob), b = run(want);
    CHECK(a.out == b.out);
}

TEST_CASE("edge gpu: notes are said once a kind, their numbers left out")
{
    // run 331: 134,527 notes of one kind, each a different text, each looked up in a growing list under one lock
    CHECK(eg::note_kind("1234 indices to cull, 1236 configured") == eg::note_kind("7 indices to cull, 9 configured"));
    CHECK(eg::note_kind("job descriptor byte 0x1a7 0x2") == "job descriptor byte # #");
    CHECK(eg::note_kind("culling from vertex 3 (taken as 0)") != eg::note_kind("7 indices to cull, 9 configured"));
}
