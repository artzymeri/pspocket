// The narrowband CELP encoder (8 kHz, multi-pulse excitation) of libcelp8enc (D-932), as its SPU program computes it.
// The algorithms are MPEG-4 Audio CELP's (ISO/IEC 14496-3 subpart 3; the reference software, ISO/IEC 14496-5: NEC's
// nec_*.c, Matsushita's pan_*.c, Philips's phi_*.c and AT&T's att_*.c, see celp_tables.cpp); the order of each
// operation, and what differs from the reference software, is the SPU program's.

#include "celp_spu_fp.hpp" // first: the floating-point pragmas

#include <algorithm>
#include <cmath>
#include <cstring>

#include "celp_codec.hpp"
#include "celp_tables.hpp"

namespace pspocket::hle::celp {

using namespace spu;

namespace {

// The SPU program's dot product of n values (its correlation routine): four vector accumulators over blocks of 16
// values, summed ((a0 + a1) + a2) + a3, then blocks of 4 and the last values into that one, then its lanes
// (l0 + l2) + (l1 + l3).
float dot_spu(const float* a, const float* b, u32 n)
{
    float acc[4][4] = {};
    u32 i = 0;
    for (; i + 16 <= n; i += 16)
        for (u32 q = 0; q < 4; q++)
            for (u32 l = 0; l < 4; l++) acc[q][l] = fma(a[i + 4 * q + l], b[i + 4 * q + l], acc[q][l]);
    float v[4];
    for (u32 l = 0; l < 4; l++) v[l] = fa(fa(fa(acc[0][l], acc[1][l]), acc[2][l]), acc[3][l]);
    for (; i + 4 <= n; i += 4)
        for (u32 l = 0; l < 4; l++) v[l] = fma(a[i + l], b[i + l], v[l]);
    for (u32 l = 0; i + l < n; l++) v[l] = fma(a[i + l], b[i + l], v[l]);
    return fa(fa(v[0], v[2]), fa(v[1], v[3]));
}

// pan_lspqtz2_dd with the 22-bit tables, Sony's way: each part's two best first-stage vectors (16 each; a distance
// summing the second dimension's term first), then for each of the two pairs both second stages, plain (DVQ: the input
// less the first stage) and predicted (PVQ: the input less the mean of the first stage and the previous quantised
// vector), each table vector added and subtracted, the distances in dimension order; each part's best pair separately,
// then whichever of DVQ and PVQ is nearer, PVQ on a tie.
void nb_quantize(const float in[10], const float prev[10], const float w[10], u32 idx[5])
{
    constexpr float kTbl = 1.0f / 32768, kD = 1.0f / 262144, kPd = 1.0f / 524288;
    constexpr u32 kNcd2[2] = {64, 32};
    u32 code[2][2];
    for (u32 p = 0; p < 2; p++) {
        float dmin[2];
        for (u32 j = 0; j < 2; j++) {
            dmin[j] = from_bits(0x4E6E6B28u); // 1e9
            code[p][j] = 255;
        }
        const float* x = in + 5 * p;
        const float* wt = w + 5 * p;
        for (u32 r = 0; r < 16; r++) {
            const s16* t = kLspNb + 5 * (16 * p + r);
            float d2[5];
            for (u32 k = 0; k < 5; k++) {
                const float d = fs(x[k], t[k] * kTbl);
                d2[k] = fm(d, d);
            }
            const float dist = fma(d2[4], wt[4], fma(d2[3], wt[3], fma(d2[2], wt[2], fma(d2[0], wt[0], fm(d2[1], wt[1])))));
            for (u32 j = 0; j < 2; j++)
                if (fcgt(dmin[j], dist)) {
                    for (u32 k = 1; k > j; k--) {
                        dmin[k] = dmin[k - 1];
                        code[p][k] = code[p][k - 1];
                    }
                    dmin[j] = dist;
                    code[p][j] = r;
                    break;
                }
        }
    }
    float dvq[2][2], pvq[2][2];
    u32 dvq_code[2][2], pvq_code[2][2];
    for (u32 j = 0; j < 2; j++) {
        float c[10], q[10];
        for (u32 k = 0; k < 10; k++) {
            const u32 p = k / 5;
            const float v = kLspNb[5 * (16 * p + code[p][j]) + k % 5] * kTbl;
            c[k] = fs(in[k], v);
            q[k] = fnms(fa(v, prev[k]), 0.5f, in[k]);
        }
        for (u32 p = 0; p < 2; p++) {
            const float inf = from_bits(0x7F800000u);
            float min_dp = inf, min_dm = inf, min_pp = inf, min_pm = inf;
            u32 at_dp = 255, at_dm = 255, at_pp = 255, at_pm = 255;
            const float* cc = c + 5 * p;
            const float* qq = q + 5 * p;
            const float* wt = w + 5 * p;
            for (u32 r = 0; r < kNcd2[p]; r++) {
                const s16* t = kDNb + 5 * (64 * p + r);
                const s16* u = kPdNb + 5 * (64 * p + r);
                float dp = 0, dm = 0, pp = 0, pm = 0;
                for (u32 k = 0; k < 5; k++) {
                    const float tk = t[k] * kD, uk = u[k] * kPd;
                    const float a = fs(cc[k], tk), b = fa(cc[k], tk), e = fs(qq[k], uk), f = fa(qq[k], uk);
                    const float a2 = fm(a, a), b2 = fm(b, b), e2 = fm(e, e), f2 = fm(f, f);
                    if (k == 0) {
                        dp = fm(a2, wt[0]);
                        dm = fm(b2, wt[0]);
                        pp = fm(e2, wt[0]);
                        pm = fm(f2, wt[0]);
                    } else {
                        dp = fma(a2, wt[k], dp);
                        dm = fma(b2, wt[k], dm);
                        pp = fma(e2, wt[k], pp);
                        pm = fma(f2, wt[k], pm);
                    }
                }
                if (fcgt(min_dp, dp)) {
                    min_dp = dp;
                    at_dp = r;
                }
                if (fcgt(min_dm, dm)) {
                    min_dm = dm;
                    at_dm = r;
                }
                if (fcgt(min_pp, pp)) {
                    min_pp = pp;
                    at_pp = r;
                }
                if (fcgt(min_pm, pm)) {
                    min_pm = pm;
                    at_pm = r;
                }
            }
            if (fcgt(min_dp, min_dm)) {
                dvq[j][p] = min_dm;
                dvq_code[j][p] = at_dm + kNcd2[p];
            } else {
                dvq[j][p] = min_dp;
                dvq_code[j][p] = at_dp;
            }
            if (fcgt(min_pp, min_pm)) {
                pvq[j][p] = min_pm;
                pvq_code[j][p] = at_pm + kNcd2[p];
            } else {
                pvq[j][p] = min_pp;
                pvq_code[j][p] = at_pp;
            }
        }
    }
    u32 best_d[2], best_p[2];
    for (u32 p = 0; p < 2; p++) {
        best_d[p] = fcgt(dvq[0][p], dvq[1][p]) ? 1 : 0;
        best_p[p] = fcgt(pvq[0][p], pvq[1][p]) ? 1 : 0;
    }
    const float dist_d = fa(dvq[best_d[0]][0], dvq[best_d[1]][1]);
    const float dist_p = fa(pvq[best_p[0]][0], pvq[best_p[1]][1]);
    if (fcgt(dist_p, dist_d)) {
        idx[0] = code[0][best_d[0]];
        idx[1] = code[1][best_d[1]];
        idx[2] = dvq_code[best_d[0]][0];
        idx[3] = dvq_code[best_d[1]][1];
        idx[4] = 0;
    } else {
        idx[0] = code[0][best_p[0]];
        idx[1] = code[1][best_p[1]];
        idx[2] = pvq_code[best_p[0]][0];
        idx[3] = pvq_code[best_p[1]][1];
        idx[4] = 1;
    }
}

} // namespace

void MpeEncoder::reset(u32 configuration)
{
    config = configuration;
    cfg = kMpeConfigs[configuration];
    std::memset(bit, 0, sizeof bit);
    mpe_pulses(configuration, bit, pos);
    frame_ctr = 0;
    n_lpc = cfg.frame_size / 80;
    std::memset(raw, 0, sizeof raw);
    std::memset(pp, 0, sizeof pp);
    {
        // The LPC analysis window: a 240-point Hamming window's first half (120 values), then a quarter of a cosine
        // over 80, in double precision.
        Nearest nearest;
        const double two_pi = 6.283185307179586;
        for (int i = 0; i < 120; i++)
            window[i] = frds(std::cos(static_cast<double>(i) * two_pi / 239.0) * -0.46 + 0.54);
        for (int i = 120; i < 200; i++) window[i] = frds(std::cos(static_cast<double>(i - 120) * two_pi / 319.0));
        for (int k = 0; k < 10; k++) {
            prev_q[k] = frds((static_cast<double>(k) + 1.0) * 0.09090909090909091);
            lsp[k] = prev_q[k];
            gamma09[k] = frds(std::pow(0.9, k + 1));
            gamma05[k] = frds(std::pow(0.5, k + 1));
        }
    }
    std::memset(lpc_a, 0, sizeof lpc_a);
    rfc0 = 0;
    std::memset(w_num, 0, sizeof w_num);
    std::memset(w_den, 0, sizeof w_den);
    std::memset(pw_mem1, 0, sizeof pw_mem1);
    std::memset(pw_mem2, 0, sizeof pw_mem2);
    std::memset(pst_wsp, 0, sizeof pst_wsp);
    vu = 0;
    std::memset(lag_cand, 0, sizeof lag_cand);
    std::memset(mem_in, 0, sizeof mem_in);
    std::memset(mem_win, 0, sizeof mem_win);
    std::memset(mem_syn, 0, sizeof mem_syn);
    std::memset(mem_wsyn, 0, sizeof mem_wsyn);
    std::memset(exc_mem, 0, sizeof exc_mem);
    std::memset(qx, 0, sizeof qx);
    pq = 0;
    rms_idx = 0;
    std::memset(renorm2, 0, sizeof renorm2);
    std::memset(lag_idx, 0, sizeof lag_idx);
    std::memset(pos_idx, 0, sizeof pos_idx);
    std::memset(sgn_idx, 0, sizeof sgn_idx);
    std::memset(gain_idx, 0, sizeof gain_idx);
    std::memset(exc, 0, sizeof exc);
}

// The LPC analysis of the frame's analysis windows: windowed, autocorrelation (lags 0 to 10) times the lag window,
// Levinson-Durbin in single precision (stopped at an unstable or vanishing order, the rest 0; silence below energy 1
// all 0); then the last one's LSPs.
void MpeEncoder::lpc_analysis()
{
    static const float kLagWindow[11] = {1.0001000f, 0.9988903f, 0.9955685f, 0.9900568f, 0.9823916f, 0.9726235f,
        0.9608164f, 0.9470474f, 0.9314049f, 0.9139889f, 0.8949091f};
    for (u32 k = 0; k < n_lpc; k++) {
        const float* x = pp + 80 * k;
        float wx[200];
        for (int i = 0; i < 200; i++) wx[i] = fm(x[i], window[i]);
        float acf[11];
        for (u32 j = 0; j < 11; j++) acf[j] = dot_spu(wx, wx + j, 200 - j);
        for (u32 j = 0; j < 11; j++) acf[j] = fm(acf[j], kLagWindow[j]);
        float P[10] = {}, Q[10] = {}, K[10] = {};
        float E = acf[0];
        if (fcgt(E, 1.0f)) {
            const float eps = fm(E, from_bits(0x358637BDu)); // 1e-6
            for (int m = 0; m < 10; m++) {
                float sum = 0;
                for (int i = 0; i < m; i++) sum = fma(P[i], acf[m - i], sum);
                const float y = fi(E, frest(E));
                const float t = fs(acf[m + 1], sum);
                const float ty = fm(t, y);
                const float kk = fma(fnms(E, y, from_bits(0x3F800001u)), ty, ty);
                K[m] = kk;
                if (!fcgt(1.0f, from_bits(bits(kk) & 0x7FFFFFFFu)) || !fcgt(E, eps)) {
                    for (int j = m; j < 10; j++) {
                        K[j] = 0;
                        Q[j] = 0;
                    }
                    break;
                }
                Q[m] = kk;
                if (m == 0) {
                    E = fm(E, fnms(kk, kk, 1.0f));
                } else {
                    for (int j = 0; j < m; j++) Q[j] = fnms(kk, P[m - 1 - j], P[j]);
                    E = fm(E, fnms(K[m], K[m], 1.0f));
                }
                for (int j = 0; j <= m; j++) P[j] = Q[j];
            }
            rfc0 = K[0];
        } else {
            rfc0 = 0;
        }
        std::memcpy(lpc_a[k], Q, sizeof Q);
    }
    float l[10];
    const int nf = lpc_to_lsf(lpc_a[n_lpc - 1], 10, l, true);
    // (Roots not found keep the last frame's values, divided by pi once more.)
    for (int i = 0; i < 10; i++) lsp[i] = fm(i < nf ? l[i] : lsp[i], from_bits(0x3EA2F983u));
}

// The LSP quantiser (nb_abs_lpc_quantizer): the weights from the LSPs' distances (each at least the minimum gap, their
// reciprocals from the estimate and one step), the indices, the quantised vector decoded from them and stabilised.
void MpeEncoder::quantize_lsp()
{
    constexpr float kGapNb = 2.0f / 256;
    float d[11], r[11], w[10];
    d[0] = lsp[0];
    for (int i = 1; i < 10; i++) d[i] = fs(lsp[i], lsp[i - 1]);
    d[10] = fs(1.0f, lsp[9]);
    for (int i = 0; i <= 10; i++) {
        const float x = fcgt(d[i], kGapNb) ? d[i] : kGapNb;
        const float y = fi(x, frest(x));
        r[i] = fma(fnms(x, y, 1.0f), y, y);
    }
    for (int i = 0; i < 10; i++) w[i] = fa(r[i], r[i + 1]);
    nb_quantize(lsp, prev_q, w, lsp_idx);
    nb_lsp_decode(lsp_idx, prev_q, q_lsp);
    // Each subframe's LPC coefficients: the quantised LSPs interpolated from the previous frame's (the last subframe's
    // the frame's own), as the decoder has them.
    const u32 n = cfg.n_subframes;
    const float pi = from_bits(0x40490FDBu);
    const float nf = csflt(static_cast<s32>(n));
    for (u32 s = 0; s + 1 < n; s++) {
        const float ratio = div_sdk(csflt(static_cast<s32>(s + 1)), nf);
        float lsf[10];
        for (int k = 0; k < 10; k++) lsf[k] = fm(fma(fs(q_lsp[k], prev_q[k]), ratio, prev_q[k]), pi);
        lsf_to_lpc10(lsf, lpc_q[s]);
    }
    std::memcpy(prev_q, q_lsp, sizeof q_lsp);
    float lsf[10];
    for (int k = 0; k < 10; k++) lsf[k] = fm(q_lsp[k], pi);
    lsf_to_lpc10(lsf, lpc_q[n - 1]);
}

namespace {

// An open-loop lag's adaptive codebook index (17 to 70 in thirds, to 89 in halves, to 144 whole; else 255).
u32 int_to_index(u32 lag)
{
    if (lag >= 17 && lag <= 70) return (lag - 17) * 3;
    if (lag >= 71 && lag <= 89) return 162 + (lag - 71) * 2;
    if (lag >= 90 && lag <= 144) return 200 + (lag - 90);
    return 255;
}

// A vector's lanes added as the SPU program adds them: (l0 + l2) + (l1 + l3).
float hsum(const float v[4]) { return fa(fa(v[0], v[2]), fa(v[1], v[3])); }

// A dot product of n values (a multiple of 8) in two vector accumulators (even and odd quads of four), then added.
float dot_pairs(const float* a, const float* b, u32 n)
{
    float e[4], o[4];
    for (u32 l = 0; l < 4; l++) {
        e[l] = fm(a[l], b[l]);
        o[l] = fm(a[4 + l], b[4 + l]);
    }
    for (u32 i = 8; i + 8 <= n; i += 8)
        for (u32 l = 0; l < 4; l++) {
            e[l] = fma(a[i + l], b[i + l], e[l]);
            o[l] = fma(a[i + 4 + l], b[i + 4 + l], o[l]);
        }
    return fa(hsum(e), hsum(o));
}
float energy_pairs(const float* x, u32 n) { return dot_pairs(x, x, n); }

// One step of the weighted synthesis filter (the LPC synthesis filter a, then the perceptual weighting filter wn/wd)
// on its two memories of twelve (past synthesis, past output; newest first), each sum three vectors of four.
float weighted_step(float x, const float* a, const float* wn, const float* wd, float* c, float* d)
{
    float p3[4], p2[4];
    for (u32 l = 0; l < 4; l++) {
        p3[l] = fma(a[8 + l], c[8 + l], fma(a[4 + l], c[4 + l], fm(a[l], c[l])));
        p2[l] = fnms(wd[8 + l], d[8 + l],
            fma(wn[8 + l], c[8 + l], fnms(wd[4 + l], d[4 + l], fma(wn[4 + l], c[4 + l], fnms(wd[l], d[l], fm(wn[l], c[l]))))));
    }
    const float xr = fa(x, hsum(p3));
    const float y = fa(xr, hsum(p2));
    std::memmove(c + 1, c, 11 * sizeof(float));
    c[0] = xr;
    std::memmove(d + 1, d, 11 * sizeof(float));
    d[0] = y;
    return y;
}

// nec_zero_filt: an excitation's weighted synthesis from zero state.
void zero_filt(const float* x, float* y, const float* a, const float* wn, const float* wd, u32 n)
{
    float c[12] = {}, d[12] = {};
    for (u32 k = 0; k < n; k++) y[k] = weighted_step(x[k], a, wn, wd, c, d);
}

} // namespace

// The perceptual weighting filters of the unquantised LPC (numerator 0.9^k, denominator 0.5^k: celp_weighting_module),
// the weighted speech, and the open-loop pitch analysis and voiced/unvoiced decision (nec_mode_decision) as the SPU
// program computes them.
void MpeEncoder::mode_decision()
{
    const u32 F = cfg.frame_size, n = cfg.n_subframes, sb = cfg.sbfrm_size;
    const u32 L = sb == 60 ? 120 : (sb > 79 ? sb : 80);
    const u32 n_pitch = F / L;
    float wsp[144 + 320];
    std::memcpy(wsp, pst_wsp, sizeof pst_wsp);
    const float ratio = div_sdk(csflt(static_cast<s32>(n_lpc)), csflt(static_cast<s32>(n)));
    for (u32 s = 0; s < n; s++) {
        const s32 k = cflts(fm(ratio, csflt(static_cast<s32>(s))));
        for (int i = 0; i < 10; i++) {
            const float a = from_bits(bits(lpc_a[k][i]) ^ 0x80000000u);
            w_den[s][i] = fm(a, gamma05[i]);
            w_num[s][i] = fm(a, gamma09[i]);
        }
        const float* x = pp + 80 + s * sb;
        for (u32 j = 0; j < sb; j++) {
            float p[4];
            for (int l = 0; l < 4; l++) {
                p[l] = fm(w_num[s][l], pw_mem1[l]);
                p[l] = fnms(w_den[s][l], pw_mem2[l], p[l]);
                p[l] = fma(w_num[s][4 + l], pw_mem1[4 + l], p[l]);
                p[l] = fnms(w_den[s][4 + l], pw_mem2[4 + l], p[l]);
                p[l] = fma(w_num[s][8 + l], pw_mem1[8 + l], p[l]);
                p[l] = fnms(w_den[s][8 + l], pw_mem2[8 + l], p[l]);
            }
            const float y = fa(x[j], hsum(p));
            std::memmove(pw_mem1 + 1, pw_mem1, 11 * sizeof(float));
            pw_mem1[0] = x[j];
            std::memmove(pw_mem2 + 1, pw_mem2, 11 * sizeof(float));
            pw_mem2[0] = y;
            wsp[144 + s * sb + j] = y;
        }
    }
    float gain_total = 0, frms_total = 0;
    const u32 spb = n / n_pitch;
    for (u32 b = 0; b < n_pitch; b++) {
        const float* x = wsp + 144 + b * L;
        // Lag 144 first, then 143 down to 17, the energy updated recursively; a shorter lag at least the minimum lag
        // away needs more than 0.75 of the best.
        float cwx = dot_spu(x - 144, x, L);
        float cwc = energy_pairs(x - 144, L);
        float max_cr = 0;
        u32 max_lag = 144;
        if (!zero_class(cwc) && !fcgt(0.0f, cwx)) {
            const float r = div_sdk(fm(cwx, cwx), cwc);
            if (fcgt(r, 0.0f)) max_cr = r;
        }
        for (u32 lag = 143; lag > 16; lag--) {
            cwx = dot_spu(x, x - lag, L);
            const float leaving = x[-static_cast<s32>(lag) - 1];
            const float entering = x[static_cast<s32>(L) - 1 - static_cast<s32>(lag)];
            cwc = fnms(leaving, leaving, cwc);
            cwc = fma(entering, entering, cwc);
            float cr = 0;
            if (!zero_class(cwc) && !fcgt(0.0f, cwx)) cr = div_sdk(fm(cwx, cwx), cwc);
            if (max_lag - lag > 16 ? fcgt(cr, fm(max_cr, 0.75f)) : fcgt(cr, max_cr)) {
                max_cr = cr;
                max_lag = lag;
            }
        }
        // The energies of the weighted block (its first four values apart) and of the input.
        float q0[4], rest[4];
        for (int l = 0; l < 4; l++) {
            q0[l] = fm(x[l], x[l]);
            rest[l] = fm(x[4 + l], x[4 + l]);
        }
        for (u32 i = 8; i + 4 <= L; i += 4)
            for (int l = 0; l < 4; l++) rest[l] = fma(x[i + l], x[i + l], rest[l]);
        const float xwx = fa(hsum(q0), hsum(rest));
        const float frms = energy_pairs(pp + 80 + b * L, L);
        float g = 1.0f;
        if (!zero_class(xwx)) {
            const float gp = fs(1.0f, div_sdk(max_cr, xwx));
            g = fcgt(from_bits(0x38D1B717u), gp) ? from_bits(0x38D1B717u) : gp; // 1e-4
        }
        frms_total = fa(frms_total, frms);
        gain_total = fma(log10_sdk(g), -10.0f, gain_total);
        for (u32 j = 0; j < spb; j++) lag_cand[b * spb + j] = int_to_index(max_lag);
    }
    const float gain = div_sdk(gain_total, csflt(static_cast<s32>(n_pitch)));
    const float frms2 = div_sdk(frms_total, csflt(static_cast<s32>(F)));
    if (!fcgt(1.5f, gain) && !fcgt(100.0f, frms2)) {
        vu = fcgt(3.5f, gain) ? 1 : fcgt(9.0f, gain) ? 2 : 3;
    } else {
        vu = fcgt(fm(gain, frms2), 93750.0f) ? 1 : 0;
    }
    std::memcpy(pst_wsp, wsp + F, sizeof pst_wsp);
}

namespace {

// The index of the largest of `count` values as the SPU program finds it: a tournament over vectors of four (in blocks
// of 32 when there are that many), a later value winning only when strictly greater; the blocks' winners compared in
// order, a later one only when strictly greater.
u32 argmax_tournament(const float* v, u32 count)
{
    // One quad's lanes: lanes 2 and 3 against 0 and 1, then lane 1 against lane 0.
    auto lanes = [](const float q[4], const u32 qi[4], float* best, u32* at) {
        float m[2];
        u32 mi[2];
        for (u32 l = 0; l < 2; l++) {
            const bool up = fcgt(q[l + 2], q[l]);
            m[l] = up ? q[l + 2] : q[l];
            mi[l] = up ? qi[l + 2] : qi[l];
        }
        const bool up = fcgt(m[1], m[0]);
        *best = up ? m[1] : m[0];
        *at = up ? mi[1] : mi[0];
    };
    // Two quads lane by lane, the second taken only when strictly greater.
    auto pick = [](const float a[4], const u32 ai[4], const float b[4], const u32 bi[4], float* m, u32* mi) {
        for (u32 l = 0; l < 4; l++) {
            const bool up = fcgt(b[l], a[l]);
            m[l] = up ? b[l] : a[l];
            mi[l] = up ? bi[l] : ai[l];
        }
    };
    const float lowest = from_bits(0xFF7FFFFEu);
    if (count > 31) {
        float best = lowest;
        u32 at = 0;
        for (u32 base = 0; base + 32 <= count; base += 32) {
            float q[8][4];
            u32 qi[8][4];
            for (u32 k = 0; k < 8; k++)
                for (u32 l = 0; l < 4; l++) {
                    q[k][l] = v[base + 4 * k + l];
                    qi[k][l] = 4 * k + l;
                }
            float m01[4], m23[4], m45[4], m67[4], m03[4], m47[4], m[4];
            u32 i01[4], i23[4], i45[4], i67[4], i03[4], i47[4], mi[4];
            pick(q[0], qi[0], q[1], qi[1], m01, i01);
            pick(q[2], qi[2], q[3], qi[3], m23, i23);
            pick(q[4], qi[4], q[5], qi[5], m45, i45);
            pick(q[6], qi[6], q[7], qi[7], m67, i67);
            pick(m01, i01, m23, i23, m03, i03);
            pick(m45, i45, m67, i67, m47, i47);
            pick(m03, i03, m47, i47, m, mi);
            float b;
            u32 bi;
            lanes(m, mi, &b, &bi);
            if (fcgt(b, best)) {
                best = b;
                at = base + bi;
            }
        }
        return at;
    }
    float q[4][4] = {};
    u32 qi[4][4];
    for (u32 k = 0; k < 4; k++)
        for (u32 l = 0; l < 4; l++) {
            q[k][l] = 4 * k + l < count ? v[4 * k + l] : 0.0f;
            qi[k][l] = 4 * k + l;
        }
    float m[4];
    u32 mi[4];
    if (count == 16) {
        float m01[4], m23[4];
        u32 i01[4], i23[4];
        pick(q[0], qi[0], q[1], qi[1], m01, i01);
        pick(q[2], qi[2], q[3], qi[3], m23, i23);
        pick(m01, i01, m23, i23, m, mi);
    } else if (count == 8) {
        pick(q[0], qi[0], q[1], qi[1], m, mi);
    } else {
        std::memcpy(m, q[0], sizeof m);
        std::memcpy(mi, qi[0], sizeof mi);
    }
    float b;
    u32 bi;
    lanes(m, mi, &b, &bi);
    return fcgt(b, lowest) ? bi : 0;
}

} // namespace

// The frame's RMS (nec_enc_rms: each subframe's, mu-law steps from the last frame's last subframe's, the index with the
// least squared error) and each subframe's normalised power (as the decoder computes it).
void MpeEncoder::quantize_rms()
{
    const u32 n = cfg.n_subframes, sb = cfg.sbfrm_size;
    float xn[4] = {};
    for (u32 s = 0; s < n; s++)
        xn[s] = sqrt_sdk(div_sdk(energy_pairs(pp + 80 + s * sb, sb), csflt(static_cast<s32>(sb))));
    const float mu = vu == 0 ? 1024.0f : 512.0f, rms_max = vu == 0 ? 7932.0f : 15864.0f;
    const float bb = div_sdk(rms_max, mu);
    const float L = log10_sdk(fa(mu, 1.0f));
    const float aa = recip_sdk(L), ib = recip_sdk(bb);
    const float nf = csflt(static_cast<s32>(n));
    float wk[4] = {}, ratio[4] = {};
    for (u32 s = 0; s < n; s++) {
        wk[s] = fm(aa, log10_sdk(fma(xn[s], ib, 1.0f)));
        ratio[s] = div_sdk(csflt(static_cast<s32>(s + 1)), nf);
    }
    const float pwk = fm(aa, log10_sdk(fma(pq, ib, 1.0f)));
    float min_er = from_bits(0x7149F2C9u); // 1e30
    u32 idx = 0;
    for (u32 k = 0; k < 64; k++) {
        const float d = fms(csflt(static_cast<s32>(k + 1)), 1.0f / 64, pwk);
        float sq[4] = {};
        for (u32 s = 0; s < n; s++) {
            const float df = fs(wk[s], fma(d, ratio[s], pwk));
            sq[s] = fm(df, df);
        }
        // (Three subframes: the first two only, as Sony's program sums.)
        const float er = n == 4 ? fa(fa(sq[0], sq[1]), fa(sq[2], sq[3])) : fa(sq[0], sq[1]);
        if (fcgt(min_er, er)) {
            min_er = er;
            idx = k;
        }
    }
    rms_idx = idx;
    const float log2_10 = log2_sdk(10.0f);
    const float d = fms(csflt(static_cast<s32>(idx + 1)), 1.0f / 64, pwk);
    for (u32 s = 0; s < n; s++) {
        const float nwk = fma(d, ratio[s], pwk);
        qx[s] = fm(bb, fs(exp2_sdk(fm(fm(nwk, L), log2_10)), 1.0f));
    }
    pq = qx[n - 1];
    for (u32 s = 0; s < n; s++) {
        float f[10], b[10];
        for (int i = 0; i < 10; i++) f[i] = from_bits(bits(lpc_q[s][i]) ^ 0x80000000u);
        float p = 1.0f;
        for (int mr = 9; mr >= 0; mr--) {
            const float dd = fnms(f[mr], f[mr], 1.0f);
            p = fm(p, dd);
            const float inv = recip_sdk(dd);
            for (int k = 0; k <= mr; k++) b[k] = f[k];
            for (int k = 0; k < mr; k++) f[k] = fm(fnms(b[mr], b[mr - 1 - k], b[k]), inv);
        }
        renorm2[s] = fm(p, fm(csflt(static_cast<s32>(sb)), fm(qx[s], qx[s])));
    }
}

// One subframe's excitation (nec_mk_target, nec_enc_acb, nec_enc_mp, nec_enc_gain) as Sony's program searches it.
void MpeEncoder::excitation(u32 s)
{
    const u32 sb = cfg.sbfrm_size, np = cfg.n_pulses;
    float a[12] = {};
    std::memcpy(a, lpc_q[s], sizeof lpc_q[s]);
    const float* wn = w_num[s];
    const float* wd = w_den[s];
    const float* x = pp + 80 + s * sb;
    const float comb_gain = vu == 2 ? 0.6f : 0.8f;

    // The target: the weighted input less the weighted synthesis filter's ringing.
    float target[80];
    {
        float c[12], d[12];
        std::memcpy(c, mem_syn, sizeof c);
        std::memcpy(d, mem_wsyn, sizeof d);
        for (u32 k = 0; k < sb; k++) {
            float h1[4], h2[4], h3[4];
            for (u32 l = 0; l < 4; l++) {
                h1[l] = fnms(wd[8 + l], mem_win[8 + l], fnms(wd[4 + l], mem_win[4 + l], fnms(wd[l], mem_win[l],
                    fma(wn[8 + l], mem_in[8 + l], fma(wn[4 + l], mem_in[4 + l], fm(wn[l], mem_in[l]))))));
                h2[l] = fnms(wd[8 + l], d[8 + l], fnms(wd[4 + l], d[4 + l], fnms(wd[l], d[l],
                    fma(wn[8 + l], c[8 + l], fma(wn[4 + l], c[4 + l], fm(wn[l], c[l]))))));
                h3[l] = fma(a[8 + l], c[8 + l], fma(a[4 + l], c[4 + l], fm(a[l], c[l])));
            }
            const float cur = fa(x[k], hsum(h1));
            const float xr = hsum(h3);
            const float fk = fa(xr, hsum(h2));
            target[k] = fs(cur, fk);
            std::memmove(mem_in + 1, mem_in, 11 * sizeof(float));
            mem_in[0] = x[k];
            std::memmove(mem_win + 1, mem_win, 11 * sizeof(float));
            mem_win[0] = cur;
            std::memmove(c + 1, c, 11 * sizeof(float));
            c[0] = xr;
            std::memmove(d + 1, d, 11 * sizeof(float));
            d[0] = fk;
        }
    }

    // The adaptive codebook: the open-loop lag's index and its two neighbours.
    float memx[230];
    std::memcpy(memx, exc_mem, sizeof exc_mem);
    const u32 op = lag_cand[s];
    u32 st, ed;
    if (static_cast<s32>(op) - 1 < 0) {
        st = 0;
        ed = 2;
    } else if (op + 1 > 254) {
        st = 252;
        ed = 254;
    } else {
        st = op - 1;
        ed = op + 1;
    }
    float acb[80], syn[80];
    mpe_acb(memx, op, sb, acb);
    zero_filt(acb, syn, a, wn, wd, sb);
    s32 lag_int, lag_frac;
    mpe_lag(op, &lag_int, &lag_frac);
    u32 best = op;
    {
        const float cwx = dot_pairs(syn, target, sb);
        float cwc_opt = energy_pairs(syn, sb), xx_opt = fm(cwx, cwx);
        for (u32 idx = st; idx <= ed; idx++) {
            float acb_c[80], syn_c[80];
            mpe_acb(memx, idx, sb, acb_c);
            zero_filt(acb_c, syn_c, a, wn, wd, sb);
            const float cwx_c = dot_pairs(syn_c, target, sb), cwc = energy_pairs(syn_c, sb);
            const float xx = fm(cwx_c, cwx_c);
            if (fcgt(fm(xx_opt, cwc), fm(cwc_opt, xx)) || !fcgt(cwx_c, 0.0f)) continue;
            best = idx;
            std::memcpy(acb, acb_c, sb * sizeof(float));
            std::memcpy(syn, syn_c, sb * sizeof(float));
            mpe_lag(idx, &lag_int, &lag_frac);
            cwc_opt = cwc;
            xx_opt = xx;
        }
    }
    lag_idx[s] = best;
    const float zsax = dot_pairs(syn, target, sb), sasa = energy_pairs(syn, sb);
    float og = 0.0f;
    if (!zero_class(sasa)) {
        const float q = div_sdk(zsax, sasa);
        og = fcgt(0.0f, q) ? 0.0f : fcgt(q, from_bits(0x3F999999u)) ? from_bits(0x3F999999u) : q; // 1.2
    }
    const bool comb = vu > 1 && lag_int > 0;

    // The multi-pulse search: the target less the adaptive codebook's part, filtered backward through the weighted
    // synthesis filter's impulse response (comb filtered for voiced frames).
    float z[80], h[80], imp[80] = {1.0f};
    for (u32 k = 0; k < sb; k++) z[k] = fnms(og, syn[k], target[k]);
    zero_filt(imp, h, a, wn, wd, sb);
    if (comb && static_cast<s32>(sb) > lag_int)
        for (u32 k = static_cast<u32>(lag_int); k < sb; k++) h[k] = fma(comb_gain, h[k - lag_int], h[k]);
    float adest[80], sgn[80];
    for (u32 i = 0; i < sb; i++) {
        const float dv = dot_spu(h, z + i, sb - i);
        const bool neg = fcgt(0.0f, dv);
        sgn[i] = neg ? -1.0f : 1.0f;
        adest[i] = neg ? from_bits(bits(dv) ^ 0x80000000u) : dv;
    }
    // The impulse response's correlations, each lag's sums from the subframe's end (twice them off the diagonal), in
    // vectors of four with a running carry; and the signed matrix.
    static thread_local float corr[80][80];
    for (u32 l = 0; l < sb; l++) {
        const u32 cnt = sb - l;
        float carry = 0.0f;
        for (u32 q = 0; q < cnt; q += 4) {
            float p[4];
            for (u32 j = 0; j < 4; j++) p[j] = q + j < cnt ? fm(h[q + j], h[q + j + l]) : 0.0f;
            float dd[4] = {fa(0.0f, p[0]), fa(p[0], p[1]), fa(p[1], p[2]), fa(p[2], p[3])};
            if (l > 0)
                for (u32 j = 0; j < 4; j++) dd[j] = fa(dd[j], dd[j]);
            const float e[4] = {fa(0.0f, dd[0]), fa(0.0f, dd[1]), fa(dd[0], dd[2]), fa(dd[1], dd[3])};
            for (u32 j = 0; j < 4; j++)
                if (q + j < 80) corr[l][q + j] = fa(e[j], carry);
            carry = fa(e[3], carry);
        }
    }
    static thread_local float M[80][80];
    for (u32 i = 0; i < sb; i++)
        for (u32 j = 0; j < sb; j++) {
            const u32 l = i > j ? i - j : j - i, m = i > j ? i : j;
            const float v = corr[l][sb - 1 - m];
            M[i][j] = sgn[i] > 0.0f ? fm(v, sgn[j]) : fnms(v, sgn[j], 0.0f);
        }

    // The pulses' search order: each by its best position's bf^2/ps, largest first (nec_alg_pre).
    float tmp[12];
    for (u32 i = 0; i < np; i++) {
        float mx = 0.0f;
        for (u32 j = 0; j < (1u << bit[i]); j++) {
            const u32 k = pos[i][j];
            const float b = zero_class(M[k][k]) ? 0.0f : div_sdk(fm(adest[k], adest[k]), M[k][k]);
            if (fcgt(b, mx)) mx = b;
        }
        tmp[i] = mx;
    }
    u32 grp[12];
    for (u32 r = 0; r < np; r++) {
        float mx = -1.0f;
        u32 at = 0xFFFFFFFFu;
        for (u32 j = 0; j < np; j++)
            if (fcgt(tmp[j], mx)) {
                mx = tmp[j];
                at = j;
            }
        grp[r] = at;
        tmp[at] = -1.0f;
    }
    // The tree search: each pulse's positions on each surviving candidate, the best kept (as many as the pulse has
    // positions, at most 32: Sony's program's pruning).
    static thread_local s32 loc[32][12], loc2[32][12];
    static thread_local float bft[1024], pst[1024], buf[1024];
    float bf[32], ps[32];
    {
        const u32 g0 = grp[0], n0 = 1u << bit[g0];
        for (u32 c = 0; c < 32; c++) {
            if (c < n0) {
                const u32 kn = pos[g0][c];
                loc[c][g0] = static_cast<s32>(kn);
                bf[c] = adest[kn];
                ps[c] = M[kn][kn];
            } else {
                loc[c][g0] = -1;
            }
        }
        u32 now = n0;
        for (u32 i = 1; i < np; i++) {
            const u32 gi = grp[i], npos = 1u << bit[gi];
            for (u32 c = 0; c < now; c++) {
                for (u32 mn = 0; mn < npos; mn++) {
                    const u32 at = c * npos + mn;
                    if (loc[c][grp[i - 1]] == -1) {
                        bft[at] = pst[at] = buf[at] = 0.0f;
                        continue;
                    }
                    const u32 kn = pos[gi][mn];
                    const float b0 = fa(bf[c], adest[kn]);
                    float p0 = fa(ps[c], M[kn][kn]);
                    for (u32 k = 0; k < i; k++) p0 = fa(p0, M[kn][loc[c][grp[k]]]);
                    bft[at] = b0;
                    pst[at] = p0;
                    buf[at] = zero_class(p0) ? 0.0f : div_sdk(fm(b0, b0), p0);
                }
            }
            const u32 product = now * npos, surv = std::min(npos, product);
            for (u32 c = 0; c < surv; c++) {
                const u32 sidx = argmax_tournament(buf, product);
                buf[sidx] = from_bits(0xFF7FFFFEu);
                bf[c] = bft[sidx];
                ps[c] = pst[sidx];
                const u32 mn = sidx & (npos - 1), src = sidx >> bit[gi];
                for (u32 k = 0; k < i; k++) loc2[c][grp[k]] = loc[src][grp[k]];
                loc2[c][gi] = pos[gi][mn];
            }
            for (u32 c = 0; c < 32; c++)
                for (u32 k = 0; k <= i; k++) loc[c][grp[k]] = c < surv ? loc2[c][grp[k]] : -1;
            now = surv;
        }
    }
    // The best candidate's pulses, each the sign of its position's correlation.
    s32 pul_loc[12];
    float pul_amp[12];
    for (u32 i = 0; i < np; i++) {
        pul_loc[i] = loc[0][i];
        pul_amp[i] = csflt(cflts(sgn[pul_loc[i]]));
    }

    // The gains (nec_enc_gain, the one candidate): the codebook pair with the least weighted error, not negative.
    float exc_mp[80] = {};
    for (u32 i = 0; i < np; i++) exc_mp[pul_loc[i]] = pul_amp[i];
    if (comb && static_cast<s32>(sb) > lag_int)
        for (u32 k = static_cast<u32>(lag_int); k < sb; k++) exc_mp[k] = fma(comb_gain, exc_mp[k - lag_int], exc_mp[k]);
    float fsc1[80];
    zero_filt(exc_mp, fsc1, a, wn, wd, sb);
    const float z_p = energy_pairs(target, sb);
    const float fzsc1 = dot_pairs(fsc1, target, sb), fsa1sc1 = dot_pairs(fsc1, syn, sb);
    const float fsc1sc1 = energy_pairs(fsc1, sb);
    const float vnorm1 = energy_pairs(acb, sb), vnorm2 = energy_pairs(exc_mp, sb);
    const float s1 = zero_class(vnorm1) ? 0.0f : sqrt_sdk(div_sdk(renorm2[s], vnorm1));
    const float s2 = zero_class(vnorm2) ? 0.0f : sqrt_sdk(div_sdk(renorm2[s], vnorm2));
    const u32 cb = (4 * (sb == 40 ? 1u : 0u) + vu) * 128;
    float min_eg = from_bits(0x7149F2C9u), nga_b = 0.0f, ngc_b = 0.0f;
    s32 best_g = -1;
    for (u32 jg = 0; jg < 64; jg++) {
        const float nga = fm(s1, static_cast<float>(kNecGain[cb + 2 * jg]) / 16384.0f);
        const float ngc = fm(s2, static_cast<float>(kNecGain[cb + 2 * jg + 1]) / 16384.0f);
        const float t115 = fms(ngc, fm(fsa1sc1, nga), fm(zsax, nga));
        const float t77 = fma(sasa, fm(nga, nga), z_p);
        const float t114 = fnms(fzsc1, ngc, t115);
        const float t70 = fma(ngc, fm(fsc1sc1, ngc), fa(t114, t114));
        const float eg = fa(t77, t70);
        if (fcgt(min_eg, eg) && !fcgt(0.0f, eg)) {
            min_eg = eg;
            best_g = static_cast<s32>(jg);
            nga_b = nga;
            ngc_b = ngc;
        }
    }
    gain_idx[s] = best_g < 0 ? 0 : static_cast<u32>(best_g);

    // The indices: each pulse's position's index in its channel, the signs (1 negative), the first pulse's highest.
    u32 pidx = 0, sidx = 0;
    for (u32 i = 0; i < np; i++) {
        const u32 npos = 1u << bit[i];
        u32 j = 0;
        while (j < npos && static_cast<s32>(pos[i][j]) != pul_loc[i]) j++;
        pidx = (pidx << bit[i]) + j;
        sidx = (sidx << 1) | (fcgt(0.0f, pul_amp[i]) ? 1u : 0u);
    }
    pos_idx[s] = pidx;
    sgn_idx[s] = sidx;

    // The excitation, the past excitation and the weighted synthesis filter's memories.
    float* e = exc + s * sb;
    for (u32 k = 0; k < sb; k++) e[k] = fma(nga_b, acb[k], fm(ngc_b, exc_mp[k]));
    std::memmove(exc_mem, exc_mem + sb, (150 - sb) * sizeof(float));
    std::memcpy(exc_mem + 150 - sb, e, sb * sizeof(float));
    for (u32 k = 0; k < sb; k++) weighted_step(e[k], a, wn, wd, mem_syn, mem_wsyn);
}

// The access unit: the LSP indices, the signal mode and RMS index, each subframe's lag, pulse positions and signs and
// gain index, most significant bit first, padded to a byte.
u32 MpeEncoder::pack(u8* au) const
{
    const u32 n = cfg.n_subframes, np = cfg.n_pulses;
    u32 pos_bits = 0;
    for (u32 i = 0; i < np; i++) pos_bits += bit[i];
    u32 nbits = 0;
    std::memset(au, 0, 64);
    auto put = [&](u32 v, u32 count) {
        for (u32 i = 0; i < count; i++, nbits++)
            if ((v >> (count - 1 - i)) & 1u) au[nbits >> 3] |= static_cast<u8>(0x80u >> (nbits & 7));
    };
    put(lsp_idx[0], 4);
    put(lsp_idx[1], 4);
    put(lsp_idx[2], 7);
    put(lsp_idx[3], 6);
    put(lsp_idx[4], 1);
    put(vu, 2);
    put(rms_idx, 6);
    for (u32 s = 0; s < n; s++) {
        put(lag_idx[s], 8);
        put(pos_idx[s], pos_bits);
        put(sgn_idx[s], np);
        put(gain_idx[s], 6);
    }
    return (nbits + 7) >> 3;
}

// The header: ".mp4", the sampling rate, the bit rate, a frame's bits (twice; the SPU program's fixed point), the
// CelpHeader's size in bits (11) and the CelpHeader (ExcitationMode 0, multi-pulse; SampleRateMode 0, 8 kHz;
// FineRateControl 0; MPE_Configuration; NumEnhLayers 0; BandwidthScalabilityMode 0), padded to a byte: 232 bits.
u32 mpe_header(u32 configuration, u8* out)
{
    // MPE_Configuration's bit rate (ISO/IEC 14496-3 subpart 3), for the configurations Sony's programs take.
    u32 bitrate = 0;
    switch (configuration) {
    case 0: bitrate = 3850; break;
    case 2: bitrate = 4650; break;
    case 6: bitrate = 5700; break;
    case 9: bitrate = 6600; break;
    case 12: bitrate = 7300; break;
    case 15: bitrate = 8700; break;
    case 18: bitrate = 9900; break;
    case 21: bitrate = 10700; break;
    case 24: bitrate = 11800; break;
    case 26: bitrate = 12200; break;
    default: return 0;
    }
    constexpr u32 kRate = 8000;
    const s32 period = static_cast<s32>(kMpeConfigs[configuration].frame_size << 15) / static_cast<s32>(kRate);
    const u32 frame_bits = static_cast<u32>(period * static_cast<s32>(bitrate) + 0x4000) >> 15;
    std::memset(out, 0, 29);
    u32 nbits = 0;
    auto put = [&](u32 v, u32 count) {
        for (u32 i = 0; i < count; i++, nbits++)
            if ((v >> (count - 1 - i)) & 1u) out[nbits >> 3] |= static_cast<u8>(0x80u >> (nbits & 7));
    };
    put(0x2E6D7034, 32);
    put(0, 8);
    put(0x4003, 16);
    put(1, 8);
    put(kRate, 32);
    put(bitrate, 32);
    put(frame_bits, 16);
    put(frame_bits, 16);
    put(0, 32);
    put(2, 8);
    put(11, 16);
    put(configuration, 8); // ExcitationMode, SampleRateMode, FineRateControl (0), MPE_Configuration
    put(0, 3);             // NumEnhLayers, BandwidthScalabilityMode
    return (nbits + 7) >> 3;
}

u32 MpeEncoder::encode_frame(const u8* pcm, u8* au)
{
    float x[320];
    for (u32 i = 0; i < cfg.frame_size; i++) {
        const u32 u = static_cast<u32>(pcm[4 * i]) << 24 | static_cast<u32>(pcm[4 * i + 1]) << 16 |
                      static_cast<u32>(pcm[4 * i + 2]) << 8 | pcm[4 * i + 3];
        x[i] = fm(from_bits(u), 32767.0f);
    }
    return encode(x, au);
}

u32 MpeEncoder::encode(const float* pcm, u8* au)
{
    const u32 F = cfg.frame_size, keep = F - 40, pp_keep = 120;
    // The input delayed (2F - 40 samples kept); its oldest frame into the analysis buffer (120 samples before it).
    // The SPU program has the reference software's DC filter (celp_preprocessing), switched off.
    std::memmove(raw, raw + F, keep * sizeof(float));
    std::memcpy(raw + keep, pcm, F * sizeof(float));
    std::memmove(pp, pp + F, pp_keep * sizeof(float));
    std::memcpy(pp + pp_keep, raw, F * sizeof(float));
    if (frame_ctr++ == 0) return 0;
    lpc_analysis();
    quantize_lsp();
    mode_decision();
    quantize_rms();
    for (u32 sf = 0; sf < cfg.n_subframes; sf++) excitation(sf);
    return pack(au);
}

} // namespace pspocket::hle::celp
