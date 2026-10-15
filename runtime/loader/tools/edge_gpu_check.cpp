// edge-gpu-check: EDGE geometry's GPU path against the SPU, on states of the game's jobs (D-988).
//
//   edge-gpu-check PROBE_DIR [--spu-rounding] [--max N] [--verbose]
//
// PROBE_DIR holds the states spu-job-replay saved at the entry and return of the EDGE stages of GTA V's EDGE build
// (--probe 0daf0 --probe 07a10 --probe 0eff8 --probe 0f5b8). For each job the stages' stand-ins (spu/edge_gpu.hpp)
// record its GPU work item from the entry states, and what they leave for the rest of the job is compared with what
// the stage left (the context's words, the return value). Then the work item runs through the GPU kernels' code on the
// CPU (runtime/rsx/metal/src/edge_kernels.inc, the same source the Metal backend compiles) and its results are compared
// with the SPU's: the output vertices (bytes), the screen-space vertices the culling used, and the kept indices.
// --spu-rounding runs the kernels' code in the SPU's floating-point environment (round toward zero, denormals flushed):
// the arithmetic then agrees with the SPU's but for its estimates (1/w, the square root), which shows the semantics
// are the same; without it the rounding is the GPU's (to nearest), which shows how far the GPU's results are.
// The probes hold the game's data: they stay in the gitignored inventory/ (CLAUDE.md rule 1).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/edge_job.h"
#include "pspocket/spu/context.h"
#include "pspocket/spu/edge_gpu.hpp"
#include "pspocket/spu/fp.h"

#include "edge_kernels.inc"

namespace eg = pspocket::spu::edge_gpu;

namespace {

constexpr uint32_t kLs = PS_SPU_LS_SIZE;

struct State {
    std::vector<uint8_t> ls;
    SPUContext ctx{};
    bool ok = false;
};

State load(const std::filesystem::path& p)
{
    State s;
    std::ifstream f(p, std::ios::binary);
    if (!f) return s;
    s.ls.resize(kLs);
    f.read(reinterpret_cast<char*>(s.ls.data()), kLs);
    f.read(reinterpret_cast<char*>(s.ctx.gpr), sizeof s.ctx.gpr);
    f.read(reinterpret_cast<char*>(&s.ctx.pc), 4);
    s.ok = static_cast<bool>(f);
    return s;
}

uint32_t ulps(float a, float b)
{
    if (a == b) return 0;
    if (std::isnan(a) || std::isnan(b)) return ~0u;
    int32_t ia, ib;
    std::memcpy(&ia, &a, 4);
    std::memcpy(&ib, &b, 4);
    if (ia < 0) ia = static_cast<int32_t>(0x80000000u - static_cast<uint32_t>(ia));
    if (ib < 0) ib = static_cast<int32_t>(0x80000000u - static_cast<uint32_t>(ib));
    const int64_t d = static_cast<int64_t>(ia) - ib;
    return static_cast<uint32_t>(std::min<int64_t>(d < 0 ? -d : d, 0xFFFFFFFF));
}

struct Totals {
    uint64_t jobs = 0, rejected = 0, leave_bad = 0;
    uint64_t verts = 0, out_bytes = 0, out_bytes_same = 0, out_verts_same = 0;
    uint64_t screen = 0, screen_same = 0, packed_same = 0, code_same = 0;
    uint32_t screen_ulps = 0, out_f32_ulps = 0, out_int_diff = 0;
    double out_f32_abs = 0, out_f32_rel = 0, screen_px = 0; // the largest differences: absolute, against |value|, pixels
    uint64_t tri = 0, kept_spu = 0, kept_gpu = 0, lists_same = 0, cull_jobs = 0;
    std::map<std::string, uint64_t> reasons;
};

// What a stage's stand-in leaves against what the stage left: the context's words, the link slot, $3.
std::string compare_leave(const State& in, const State& out, void (*leave)(const eg::Recording&, uint8_t*, SPUContext&),
    const eg::Recording& r)
{
    std::vector<uint8_t> ls = in.ls;
    SPUContext ctx = in.ctx;
    leave(r, ls.data(), ctx);
    std::string d;
    for (uint32_t a = r.ctx; a < r.ctx + 0x180; a += 4)
        if (std::memcmp(ls.data() + a, out.ls.data() + a, 4) != 0)
            d += std::format(" ctx+{:#x} {:08x} want {:08x};", a - r.ctx, ps_ld32(ls.data() + a), ps_ld32(out.ls.data() + a));
    const uint32_t sp = in.ctx.gpr[1].u32[3];
    if (std::memcmp(ls.data() + sp + 0x10, out.ls.data() + sp + 0x10, 16) != 0) d += " link slot;";
    if (ctx.gpr[3].u32[3] != out.ctx.gpr[3].u32[3])
        d += std::format(" $3 {:#x} want {:#x};", ctx.gpr[3].u32[3], out.ctx.gpr[3].u32[3]);
    if (ctx.pc != (out.ctx.pc & PS_SPU_PC_MASK)) d += std::format(" pc {:#x} want {:#x};", ctx.pc, out.ctx.pc);
    return d;
}

} // namespace

int main(int argc, char** argv)
{
    std::string dir;
    bool spu_rounding = false, verbose = false;
    uint64_t max = 0;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--spu-rounding") spu_rounding = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--max" && i + 1 < argc) max = std::strtoull(argv[++i], nullptr, 0);
        else if (dir.empty()) dir = a;
        else {
            std::fprintf(stderr, "usage: edge-gpu-check PROBE_DIR [--spu-rounding] [--max N] [--verbose]\n");
            return 64;
        }
    }
    if (dir.empty()) {
        std::fprintf(stderr, "usage: edge-gpu-check PROBE_DIR [--spu-rounding] [--max N] [--verbose]\n");
        return 64;
    }
    std::map<uint64_t, bool> seqs;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string n = e.path().filename().string();
        if (n.find("-0daf0-0-in.bin") != std::string::npos) seqs[std::strtoull(n.c_str(), nullptr, 10)] = true;
    }
    Totals t;
    const auto path = [&](uint64_t seq, const char* fn, const char* io) {
        return std::filesystem::path(dir) / std::format("{}-{}-0-{}.bin", seq, fn, io);
    };
    for (const auto& [seq, unused] : seqs) {
        (void)unused;
        if (max && t.jobs >= max) break;
        const State dec_in = load(path(seq, "0daf0", "in")), dec_out = load(path(seq, "0daf0", "out"));
        const State skin_in = load(path(seq, "07a10", "in")), skin_out = load(path(seq, "07a10", "out"));
        const State cull_in = load(path(seq, "0eff8", "in")), cull_out = load(path(seq, "0eff8", "out"));
        const State comp_in = load(path(seq, "0f5b8", "in")), comp_out = load(path(seq, "0f5b8", "out"));
        if (!dec_in.ok || !dec_out.ok || !cull_in.ok || !cull_out.ok || !comp_in.ok || !comp_out.ok) continue;
        t.jobs++;
        eg::Recording r;
        r.clear();
        // The stand-ins run in the SPU's environment, as in the game.
        const uint64_t token = ps_spu_fpenv_enter();
        bool ok = eg::on_decompress(r, dec_in.ls.data(), dec_in.ctx);
        std::string leaves;
        if (ok) leaves += compare_leave(dec_in, dec_out, eg::leave_decompress, r);
        const uint32_t skin = dec_in.ls[r.ctx + 6] & 15u;
        if (ok && skin && skin_in.ok) {
            ok = eg::on_skin(r, skin_in.ls.data(), skin_in.ctx);
            if (ok) leaves += compare_leave(skin_in, skin_out, eg::leave_skin, r);
        }
        if (ok) ok = eg::on_cull(r, cull_in.ls.data(), cull_in.ctx);
        if (ok) ok = eg::on_compress(r, comp_in.ls.data(), comp_in.ctx);
        if (ok) leaves += compare_leave(comp_in, comp_out, eg::leave_compress, r);
        ps_spu_fpenv_leave(token);
        if (!ok) {
            t.rejected++;
            t.reasons[r.reject]++;
            if (verbose) std::printf("job %llu: not for the GPU: %s\n", static_cast<unsigned long long>(seq), r.reject.c_str());
            continue;
        }
        if (!leaves.empty()) {
            t.leave_bad++;
            std::printf("job %llu: the stand-ins leave another state:%s\n", static_cast<unsigned long long>(seq), leaves.c_str());
        }
        r.finish();
        const ps_edge_job& j = *reinterpret_cast<const ps_edge_job*>(r.blob.data());

        // The kernels' code over the work item.
        const uint64_t token2 = spu_rounding ? ps_spu_fpenv_enter() : 0;
        std::vector<uint8_t> out(j.num_vertexes * j.out_stride + 16, 0);
        std::vector<ek_screen> scr(j.num_vertexes);
        for (uint32_t v = 0; v < j.num_vertexes; v++) scr[v] = ek_vertex(r.blob.data(), &j, v, out.data() + v * j.out_stride);
        std::vector<uint16_t> kept;
        const uint8_t* idx = r.blob.data() + j.indices;
        for (uint32_t i = 0; i + 2 < j.num_indexes; i += 3) {
            const uint32_t a = ps_ld16(idx + 2 * i), b = ps_ld16(idx + 2 * i + 2), c = ps_ld16(idx + 2 * i + 4);
            if (!ek_culled(&j, scr[a], scr[b], scr[c])) {
                kept.push_back(static_cast<uint16_t>(a));
                kept.push_back(static_cast<uint16_t>(b));
                kept.push_back(static_cast<uint16_t>(c));
            }
        }
        if (spu_rounding) ps_spu_fpenv_leave(token2);

        // --verbose: the skinned normals and tangents against the tables the skinning left (without its state, the
        // decompressed ones against the decompression's).
        if (verbose && j.uv_data) {
            const bool decode_only = !skin_out.ok; // without the skinning's state: the decompression's
            const State& tabs = skin && skin_out.ok && !decode_only ? skin_out : dec_out;
            const uint64_t tok = spu_rounding ? ps_spu_fpenv_enter() : 0;
            uint32_t bad = 0, worst = 0;
            for (uint32_t v = 0; v < j.num_vertexes; v++) {
                for (uint32_t which = 0; which < 2; which++) {
                    const uint32_t off = which ? j.tan_offset : j.nrm_offset;
                    if (off == ~0u) continue;
                    const uint8_t* q = r.blob.data() + j.uv_data + v * j.uv_stride + off;
                    float vec[4];
                    ek_unit_vector((uint32_t(q[0]) << 16) | (uint32_t(q[1]) << 8) | q[2], vec);
                    if (j.skin && !decode_only) ek_skin_vector(vec, ek_skin_rows(r.blob.data(), &j, v));
                    const uint32_t table = ps_ld32(tabs.ls.data() + r.ctx + 0x58 + 4 * (which ? 3 : 2));
                    for (uint32_t k = 0; k < 4; k++) {
                        float want;
                        const uint32_t u = ps_ld32(tabs.ls.data() + ((table + 16 * v + 4 * k) & PS_SPU_LS_MASK));
                        std::memcpy(&want, &u, 4);
                        const uint32_t d = ulps(want, vec[k]);
                        if (d) bad++;
                        worst = std::max(worst, d);
                    }
                }
            }
            if (spu_rounding) ps_spu_fpenv_leave(tok);
            if (bad) std::printf("job %llu: %u normal and tangent components differ (at most %u ulps), skinning %u\n",
                static_cast<unsigned long long>(seq), bad, worst, j.skin);
        }

        // Against the SPU: the output vertices.
        const uint32_t out_at = comp_out.ctx.gpr[3].u32[3];
        bool all_same = true;
        uint32_t first_bad = ~0u;
        for (uint32_t v = 0; v < j.num_vertexes; v++) {
            const uint8_t* want = comp_out.ls.data() + ((out_at + v * j.out_stride) & PS_SPU_LS_MASK);
            const uint8_t* got = out.data() + v * j.out_stride;
            t.verts++;
            const bool same = std::memcmp(want, got, j.out_stride) == 0;
            t.out_verts_same += same;
            if (!same && first_bad == ~0u) first_bad = v;
            all_same &= same;
            for (uint32_t a = 0; a < j.out_count; a++) {
                const ps_edge_out_attr& o = j.out[a];
                if (o.format == PS_EDGE_FMT_F32) {
                    for (uint32_t k = 0; k < o.components; k++) {
                        float fw, fg;
                        const uint32_t uw = ps_ld32(want + o.offset + 4 * k), ug = ps_ld32(got + o.offset + 4 * k);
                        std::memcpy(&fw, &uw, 4);
                        std::memcpy(&fg, &ug, 4);
                        t.out_f32_ulps = std::max(t.out_f32_ulps, ulps(fw, fg));
                        t.out_f32_abs = std::max(t.out_f32_abs, static_cast<double>(std::fabs(fw - fg)));
                        if (fw != 0) t.out_f32_rel = std::max(t.out_f32_rel, std::fabs(static_cast<double>(fw) - static_cast<double>(fg)) / std::fabs(static_cast<double>(fw)));
                    }
                } else if (o.format == PS_EDGE_FMT_X11Y11Z10N) {
                    const uint32_t uw = ps_ld32(want + o.offset), ug = ps_ld32(got + o.offset);
                    const int bits[3] = {11, 11, 10};
                    int shift = 0;
                    for (int k = 0; k < 3; k++) {
                        const int m = (1 << bits[k]) - 1;
                        const int dw = static_cast<int>((uw >> shift) & static_cast<uint32_t>(m)), dg = static_cast<int>((ug >> shift) & static_cast<uint32_t>(m));
                        t.out_int_diff = std::max<uint32_t>(t.out_int_diff, static_cast<uint32_t>(std::abs(dw - dg)));
                        shift += bits[k];
                    }
                } else if (o.format == PS_EDGE_FMT_I16N) {
                    for (uint32_t k = 0; k < o.components; k++) {
                        const int dw = ps_ld16(want + o.offset + 2 * k), dg = ps_ld16(got + o.offset + 2 * k);
                        t.out_int_diff = std::max<uint32_t>(t.out_int_diff, static_cast<uint32_t>(std::abs(dw - dg)));
                    }
                }
            }
        }
        if (verbose && !all_same) {
            const uint8_t* want = comp_out.ls.data() + ((out_at + first_bad * j.out_stride) & PS_SPU_LS_MASK);
            std::string w, g;
            for (uint32_t i = 0; i < j.out_stride; i++) {
                w += std::format("{:02x}", want[i]);
                g += std::format("{:02x}", out[first_bad * j.out_stride + i]);
            }
            std::printf("job %llu vertex %u: SPU %s GPU %s\n", static_cast<unsigned long long>(seq), first_bad, w.c_str(), g.c_str());
        }

        // The screen-space vertices the culling used (the callback's output).
        const uint32_t c = r.ctx;
        const uint32_t which = (cull_out.ls[c] & 15u);
        const uint32_t sbuf = ps_ld32(cull_out.ls.data() + c + 0x10 + 4 * which);
        for (uint32_t v = 0; v < j.num_vertexes; v++) {
            const uint8_t* s = cull_out.ls.data() + ((sbuf + 16 * v) & PS_SPU_LS_MASK);
            float x, y;
            const uint32_t ux = ps_ld32(s), uy = ps_ld32(s + 4);
            std::memcpy(&x, &ux, 4);
            std::memcpy(&y, &uy, 4);
            const uint32_t code = ps_ld32(s + 8), packed = ps_ld32(s + 12);
            t.screen++;
            const uint32_t d = std::max(ulps(x, scr[v].x), ulps(y, scr[v].y));
            t.screen_ulps = std::max(t.screen_ulps, d);
            if (packed >> 31) // in front of the camera (w > 0): where its screen position means something
                t.screen_px = std::max(t.screen_px, std::max(std::fabs(static_cast<double>(x) - static_cast<double>(scr[v].x)), std::fabs(static_cast<double>(y) - static_cast<double>(scr[v].y))));
            t.screen_same += d == 0;
            t.code_same += (code & 0xFC000000u) == scr[v].code;
            t.packed_same += packed == scr[v].packed;
        }

        // The kept indices.
        if (j.cull_mode) {
            t.cull_jobs++;
            const uint32_t n = cull_out.ctx.gpr[3].u32[3];
            const uint32_t at = ps_ld32(cull_out.ls.data() + c + 0x50);
            std::vector<uint16_t> want(n);
            for (uint32_t i = 0; i < n; i++) want[i] = static_cast<uint16_t>(ps_ld16(cull_out.ls.data() + ((at + 2 * i) & PS_SPU_LS_MASK)));
            t.tri += j.num_indexes / 3;
            t.kept_spu += n / 3;
            t.kept_gpu += kept.size() / 3;
            t.lists_same += want == kept;
            if (verbose && want != kept) {
                // the same rules over the SPU's own screen-space vertices
                std::vector<ek_screen> spu(j.num_vertexes);
                for (uint32_t v = 0; v < j.num_vertexes; v++) {
                    const uint8_t* q = cull_out.ls.data() + ((sbuf + 16 * v) & PS_SPU_LS_MASK);
                    const uint32_t ux = ps_ld32(q), uy = ps_ld32(q + 4);
                    std::memcpy(&spu[v].x, &ux, 4);
                    std::memcpy(&spu[v].y, &uy, 4);
                    spu[v].code = ps_ld32(q + 8) & 0xFC000000u;
                    spu[v].packed = ps_ld32(q + 12);
                }
                uint32_t k2 = 0;
                for (uint32_t i = 0; i + 2 < j.num_indexes; i += 3)
                    k2 += !ek_culled(&j, spu[ps_ld16(idx + 2 * i)], spu[ps_ld16(idx + 2 * i + 2)], spu[ps_ld16(idx + 2 * i + 4)]);
                std::printf("job %llu: kept %zu indices, the SPU %u; over the SPU's vertices %u; mode %u flip %x culled %u%u%u%u nopix %u\n",
                    static_cast<unsigned long long>(seq), kept.size(), n, 3 * k2, j.cull_mode, j.flip, j.culled[0], j.culled[1],
                    j.culled[2], j.culled[3], j.nopix);
            }
        }
    }
    std::printf("%llu jobs: %llu not for the GPU, %llu with stand-ins leaving another state\n",
        static_cast<unsigned long long>(t.jobs), static_cast<unsigned long long>(t.rejected), static_cast<unsigned long long>(t.leave_bad));
    for (const auto& [why, n] : t.reasons) std::printf("  not for the GPU (%llu): %s\n", static_cast<unsigned long long>(n), why.c_str());
    std::printf("output vertices: %llu of %llu the same bytes; F32 at most %u ulps apart (%.3g absolute, %.3g of the value), the "
                "integer formats %u\n", static_cast<unsigned long long>(t.out_verts_same), static_cast<unsigned long long>(t.verts),
        t.out_f32_ulps, t.out_f32_abs, t.out_f32_rel, t.out_int_diff);
    std::printf("screen vertices: %llu of %llu the same x and y (at most %u ulps; %.3g pixels in front of the camera), outcodes "
                "%llu, integer coordinates %llu\n",
        static_cast<unsigned long long>(t.screen_same), static_cast<unsigned long long>(t.screen), t.screen_ulps, t.screen_px,
        static_cast<unsigned long long>(t.code_same), static_cast<unsigned long long>(t.packed_same));
    std::printf("culling: %llu jobs, %llu triangles, kept %llu by the SPU and %llu here; %llu index lists the same\n",
        static_cast<unsigned long long>(t.cull_jobs), static_cast<unsigned long long>(t.tri), static_cast<unsigned long long>(t.kept_spu),
        static_cast<unsigned long long>(t.kept_gpu), static_cast<unsigned long long>(t.lists_same));
    return 0;
}
