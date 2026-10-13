// The wideband CELP encoder (16 kHz, regular pulse excitation) of libcelpenc (D-932), as its SPU program computes it.
// The algorithms are the MPEG-4 Audio reference encoder's (ISO/IEC 14496-5: celp_encoder.c, phi_lpc.c, phi_axit.c,
// phi_apre.c, phi_xits.c by Philips Research, pan_lspqtz2_dd.c by Matsushita, see celp_tables.cpp); the order of each
// operation, its precision and what differs from the reference software are the SPU program's.

#include "celp_spu_fp.hpp" // first: the floating-point pragmas

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "celp_codec.hpp"
#include "celp_tables.hpp"

namespace pspocket::hle::celp {

using namespace spu;

namespace {

constexpr float kFltMin = 1.17549435e-38f; // 0x00800000
constexpr u32 kNotMax = 0xFF7FFFFEu;        // the searches' "nothing yet" (one step above -FLT_MAX)

// The weighting filter's gamma (0.8) to the powers 1 to 20, each rounded once to the nearest float (the SPU program's
// constants).
const float* gammas08()
{
    static const auto table = [] {
        Nearest nearest;
        std::array<float, 20> g{};
        double p = 1.0;
        for (int k = 0; k < 20; k++) {
            p *= 0.8;
            g[static_cast<usize>(k)] = static_cast<float>(p);
        }
        return g;
    }();
    return table.data();
}

// sum c[k] m[k] over 20 coefficients the way the SPU program's filters add them: four lanes of five products each (a
// lane's products in order), then the lanes pairwise.
float sum20(const float* c, const float* m)
{
    float l[4];
    for (int j = 0; j < 4; j++) {
        float v = fm(c[j], m[j]);
        for (int g = 1; g < 5; g++) v = fma(c[4 * g + j], m[4 * g + j], v);
        l[j] = v;
    }
    return fa(fa(l[0], l[2]), fa(l[1], l[3]));
}

void push(float* mem, float v)
{
    for (int k = 19; k > 0; k--) mem[k] = mem[k - 1];
    mem[0] = v;
}

// The four lanes of a dot product over a subframe (24, 30 or 40 samples: 6, 8 or 10 vectors of four, the arrays zero
// beyond the subframe on one side at least), the SPU program's grouping: vectors 0 to 2, 3 to 5 and 6 to the last
// summed apart, then together.
void lanes(const float* a, const float* b, u32 vecs, float l[4])
{
    for (u32 j = 0; j < 4; j++) {
        float a0 = fm(a[j], b[j]);
        a0 = fma(a[4 + j], b[4 + j], a0);
        a0 = fma(a[8 + j], b[8 + j], a0);
        float a1 = fm(a[12 + j], b[12 + j]);
        a1 = fma(a[16 + j], b[16 + j], a1);
        a1 = fma(a[20 + j], b[20 + j], a1);
        float sum = fa(a0, a1);
        if (vecs > 6) {
            float a2 = fm(a[24 + j], b[24 + j]);
            for (u32 g = 7; g < vecs; g++) a2 = fma(a[4 * g + j], b[4 * g + j], a2);
            sum = fa(sum, a2);
        }
        l[j] = sum;
    }
}

float dot(const float* a, const float* b, u32 vecs)
{
    float l[4];
    lanes(a, b, vecs, l);
    return fa(fa(l[0], l[2]), fa(l[1], l[3]));
}

// Backward filtering (PHI_backward_filtering): out[n] = sum over k >= n of h[k - n] x[k]; x zero beyond the subframe.
void backward(const float* x, const float* h, u32 sb, float* out)
{
    const u32 vecs = (sb + 3) / 4;
    for (u32 n = 0; n < 4 * vecs; n++) out[n] = 0;
    for (u32 n = 0; n < sb; n++) {
        float hs[40];
        for (u32 k = 0; k < 4 * vecs; k++) hs[k] = k >= n && k - n < sb ? h[k - n] : 0.0f;
        out[n] = dot(hs, x, vecs);
    }
}

// Convolution with the impulse response (the adaptive codebook's candidates): y[i] = sum over j <= i of h[i - j] v[j].
void convolve(const float* h, const float* v, u32 sb, float* y)
{
    const u32 vecs = (sb + 3) / 4;
    for (u32 i = 0; i < sb; i++) {
        float hr[40];
        for (u32 k = 0; k < 4 * vecs; k++) hr[k] = k <= i ? h[i - k] : 0.0f;
        y[i] = dot(hr, v, vecs);
    }
}

// Which of 16 values (four vectors of four) is largest, the SPU program's tournament: each lane's first pair, its
// second pair, the pairs' winners; then lanes 0 and 2, 1 and 3, and the two winners. A later value wins only when
// strictly larger.
u32 argmax16(const float* v)
{
    u32 lane_idx[4];
    float lane_val[4];
    for (u32 j = 0; j < 4; j++) {
        const u32 i1 = fcgt(v[4 + j], v[j]) ? 4 + j : j;
        const u32 i2 = fcgt(v[12 + j], v[8 + j]) ? 12 + j : 8 + j;
        const u32 i = fcgt(v[i2], v[i1]) ? i2 : i1;
        lane_idx[j] = i;
        lane_val[j] = v[i];
    }
    for (u32 j = 0; j < 2; j++)
        if (fcgt(lane_val[j + 2], lane_val[j])) {
            lane_val[j] = lane_val[j + 2];
            lane_idx[j] = lane_idx[j + 2];
        }
    return fcgt(lane_val[1], lane_val[0]) ? lane_idx[1] : lane_idx[0];
}

} // namespace

// One subframe's excitation (celp_excitation_analysis with the LPC residual and weighting before it), as libcelpenc's
// SPU program computes it for 40-sample subframes.
void RpeEncoder::excitation(u32 s)
{
    const u32 sb = cfg.sbfrm_size, D = cfg.D, Np = cfg.Np;
    const u32 vecs = (sb + 3) / 4;
    const float* x = buffer + (buffer_len - 3 * cfg.frame_size) + s * sb;
    const float* a = ap[s];
    float aw[20];
    for (int k = 0; k < 20; k++) aw[k] = fm(a[k], gammas08()[k]);

    // The residual, perceptually weighted, the weighted filter's impulse response, its zero input response and the
    // target (the arrays zero beyond the subframe).
    float hst[20] = {}, zst[20], t[40] = {}, h[40] = {};
    std::memcpy(zst, s_state, sizeof zst);
    for (u32 n = 0; n < sb; n++) {
        const float r = fs(x[n], sum20(a, mem_i));
        const float vo = fa(r, sum20(aw, wden));
        const float hn = fa(n == 0 ? 1.0f : 0.0f, sum20(aw, hst));
        const float z = sum20(aw, zst);
        t[n] = fs(vo, z);
        h[n] = hn;
        push(mem_i, x[n]);
        push(wden, vo);
        push(hst, hn);
        push(zst, z);
    }
    float tb[40];
    backward(t, h, sb, tb);

    // The adaptive codebook's energy table, a row for each subframe's length of the codebook: the rows move down a
    // subframe, the first row new (the reference software recomputes every row in the frame's middle subframe; the SPU
    // program never does).
    const u32 rows = 256 / sb;
    const u32 lm = 1 + (sb - 1) / 3;
    for (u32 r = rows - 1; r > 0; r--) std::memcpy(e_tbl[r], e_tbl[r - 1], sizeof e_tbl[r]);
    for (u32 j = 0; j < lm; j++) {
        const u32 l = 255 - 3 * j;
        float v = acb[l];
        float e = fma(v, v, kFltMin);
        for (u32 k = 1; k < sb; k++) {
            v = fma(afp, v, acb[l + k]);
            e = fma(v, v, e);
        }
        e_tbl[0][j] = div_gcc(1.0f, e);
    }
    // The correlations with the backward filtered target, weighted by the energies.
    for (u32 i = 0; i < rows; i++)
        for (u32 j = 0; j < lm; j++) {
            const u32 l = 255 - sb * i - 3 * j;
            const float v = dot(acb + l, tb, vecs);
            rap[i][j] = fm(fm(v, v), e_tbl[i][j]);
        }
    // Five lags, each with its two neighbours: fifteen candidates. For 40-sample subframes the SPU program compares
    // each row's 16 values as a tournament, for shorter ones it scans the row's values in order; a later value wins
    // only when strictly larger.
    u32 cand[15];
    {
        u32 row = 0, col = 0;
        for (u32 k = 0; k < 5; k++) {
            float best = from_bits(kNotMax);
            if (sb == 40) {
                u32 at = (row << 16) | col;
                for (u32 r = 0; r < rows; r++) {
                    const u32 c = argmax16(rap[r]);
                    if (fcgt(rap[r][c], best)) {
                        best = rap[r][c];
                        at = (r << 16) | c;
                    }
                }
                row = at >> 16;
                col = at & 15;
            } else {
                for (u32 r = 0; r < rows; r++)
                    for (u32 c = 0; c < lm; c++)
                        if (fcgt(rap[r][c], best)) {
                            best = rap[r][c];
                            row = r;
                            col = c;
                        }
            }
            const u32 is = row * sb + 3 * col;
            const u32 first = is == 0 ? 0 : is - 1;
            cand[3 * k] = first;
            cand[3 * k + 1] = first + 1;
            cand[3 * k + 2] = first + 2;
            rap[row][col] = from_bits(kNotMax);
        }
    }
    // The adaptive codebook search. The candidates' filtered vectors have whole vectors of four: for 30-sample subframes
    // the last two values are the codebook's next two samples after a full convolution and the previous candidate's
    // last two values shifted on after the end correction, and their squares count in the energy (the target is zero
    // there). The SPU program's end correction for 30-sample subframes also skips the seventh vector: values 24 to 27
    // stay the previous candidate's.
    float rs = from_bits(kNotMax), g = 0;
    u32 ks = 0;
    {
        float y[40] = {}, yp[40] = {};
        u32 prevm = 0;
        for (u32 k = 0; k < 15; k++) {
            const u32 m = 255 - cand[k];
            if (k > 0 && prevm - m == 1) {
                y[0] = fm(acb[m], h[0]);
                for (u32 i = 1; i < 4 * vecs; i++) y[i] = fma(acb[m], h[i], yp[i - 1]);
                if (sb == 30)
                    for (u32 i = 24; i < 28; i++) y[i] = yp[i];
            } else {
                float seg[40] = {};
                for (u32 i = 0; i < sb; i++) seg[i] = acb[m + i];
                convolve(h, seg, sb, y);
                for (u32 i = sb; i < 4 * vecs; i++) y[i] = acb[m + i];
            }
            std::memcpy(yp, y, sizeof y);
            prevm = m;
            float ln[4], ld[4];
            lanes(y, t, vecs, ln);
            lanes(y, y, vecs, ld);
            const float num = fa(fa(ln[0], ln[2]), fa(ln[1], ln[3]));
            const float den = fa(fa(fa(ld[0], ld[2]), kFltMin), fa(ld[1], ld[3]));
            const float q = div_gcc(num, den);
            const float r = fm(num, q);
            if (fcgt(r, rs)) {
                rs = r;
                ks = k;
                g = q;
            }
        }
    }
    lag[s] = cand[ks];
    // Its gain quantised.
    {
        const bool neg = fcgt(0.0f, g);
        const float ga = from_bits(bits(g) & 0x7FFFFFFFu);
        u32 j = 0;
        while (j < 31 && fcgt(ga, kCbaDec[j])) j++;
        again_idx[s] = neg ? (~j & 63) : j;
        g = neg ? from_bits(bits(kCbaGain[j]) ^ 0x80000000u) : kCbaGain[j];
    }
    const float again = g;
    const u32 m = 255 - lag[s];
    float acb_part[40] = {}, v[40] = {}, e[40] = {}, eb[40] = {};
    for (u32 k = 0; k < sb; k++) acb_part[k] = acb[m + k];
    convolve(h, acb_part, sb, v);
    for (u32 k = 0; k < sb; k++) e[k] = fnms(again, v[k], t[k]);
    backward(e, h, sb, eb);

    // The fixed codebook: the phase whose pulses' backward filtered target is largest.
    float mag[40] = {};
    for (u32 k = 0; k < sb; k++) mag[k] = from_bits(bits(eb[k]) & 0x7FFFFFFFu);
    u32 phase = 0;
    if (D == 8) {
        float sums[8];
        for (u32 j = 0; j < 4; j++) {
            sums[j] = fa(fa(fa(mag[j], mag[8 + j]), fa(mag[16 + j], mag[24 + j])), mag[32 + j]);
            sums[4 + j] = fa(fa(fa(mag[4 + j], mag[12 + j]), fa(mag[20 + j], mag[28 + j])), mag[36 + j]);
        }
        u32 li[4];
        float lv[4];
        for (u32 j = 0; j < 4; j++) {
            li[j] = fcgt(sums[4 + j], sums[j]) ? 4 + j : j;
            lv[j] = sums[li[j]];
        }
        for (u32 j = 0; j < 2; j++)
            if (fcgt(lv[j + 2], lv[j])) {
                lv[j] = lv[j + 2];
                li[j] = li[j + 2];
            }
        phase = fcgt(lv[1], lv[0]) ? li[1] : li[0];
    } else {
        float vs = from_bits(kNotMax);
        for (u32 p = 0; p < D; p++) {
            float sum = 0;
            for (u32 j = p; j < sb; j += D) sum = fa(sum, mag[j]);
            if (fcgt(sum, vs)) {
                vs = sum;
                phase = p;
            }
        }
    }
    // The pulses' signs, and which are fixed.
    s32 amp[6];
    float dtmp[6];
    u32 nonzero = 0;
    for (u32 i = 0; i < Np; i++) {
        const float vv = eb[phase + i * D];
        amp[i] = fcgt(vv, 0.0f) ? 1 : fcgt(0.0f, vv) ? -1 : 0;
        dtmp[i] = mag[phase + i * D];
        if (!zero_class(dtmp[i])) nonzero++;
    }
    const u32 nz = Np - 4;
    u32 pos[6] = {};
    if (static_cast<s32>(nonzero) > static_cast<s32>(Np) - 3) {
        for (u32 i = 0; i < nz; i++) {
            float mx = 0;
            u32 at = 0;
            for (u32 l = 0; l < Np; l++)
                if (fcgt(dtmp[l], mx)) {
                    mx = dtmp[l];
                    at = l;
                }
            pos[at] = 1;
            dtmp[at] = 0;
        }
    } else {
        for (u32 i = 0; i < Np; i++)
            if (fcgt(dtmp[i], 0.0f)) pos[i] = 1;
        for (u32 i = 0, l = nonzero; l < nz; i++)
            if (zero_class(dtmp[i])) {
                pos[i] = 1;
                l++;
            }
    }
    // The local fixed codebook: the free pulses each kept or dropped.
    s32 cb[16][40] = {};
    for (u32 i = 0; i < Np; i++) cb[0][phase + i * D] = amp[i];
    for (u32 i = 0, mm = 1; i < Np; i++) {
        if (pos[i]) continue;
        const u32 index = phase + i * D;
        for (u32 j = 0, k = mm; j < k; j++) {
            for (u32 nn = phase; nn < sb; nn += D) cb[mm][nn] = cb[j][nn];
            cb[mm++][index] = 0;
        }
    }
    // Five candidates by the preselection ratio.
    float rfp[16];
    for (u32 i = 0; i < 16; i++) {
        float t1 = 0, en = kFltMin;
        for (u32 j = 0; j < sb; j++) {
            t1 = fma(t1, afp, csflt(cb[i][j]));
            en = fma(t1, t1, en);
        }
        float t2 = 0;
        bool first = true;
        for (u32 j = phase; j < sb; j += D) {
            t2 = first ? fm(csflt(cb[i][j]), eb[j]) : fma(csflt(cb[i][j]), eb[j], t2);
            first = false;
        }
        rfp[i] = div_gcc(fm(t2, t2), en);
    }
    u32 pick[5];
    for (u32 k = 0; k < 5; k++) {
        pick[k] = argmax16(rfp);
        rfp[pick[k]] = from_bits(kNotMax);
    }
    // The fixed codebook search with its gain quantised.
    float best_r = from_bits(kNotMax), fgain = 0;
    u32 best_k = 0;
    for (u32 k = 0; k < 5; k++) {
        const s32* c = cb[pick[k]];
        float num = 0, den = kFltMin;
        for (u32 i = 0; i < sb; i++) {
            float y = 0;
            if (i >= phase) {
                bool first = true;
                for (u32 j = phase; j <= i; j += D) {
                    y = first ? fm(csflt(c[j]), h[i - j]) : fma(csflt(c[j]), h[i - j], y);
                    first = false;
                }
            }
            num = fma(y, e[i], num);
            den = fa(den, fm(y, y));
        }
        float q = div_gcc(num, den);
        u32 mq = 0;
        if (s == 0) {
            while (mq < 30 && fcgt(q, kCbfDec[mq])) mq++;
            q = kCbfGain[mq];
        } else {
            const float qq = div_gcc(q, gp);
            while (mq < 7 && fcgt(qq, kCbfDiffDec[mq])) mq++;
            q = fm(gp, kCbfDiffGain[mq]);
        }
        const float r = fms(num, fa(q, q), fm(den, fm(q, q)));
        if (fcgt(r, best_r)) {
            best_r = r;
            best_k = k;
            fgain_idx[s] = mq;
            fgain = q;
        }
    }
    gp = fgain;
    // The shape index: the chosen vector's pulses as ternary digits, times the spacing, plus the phase.
    s32 code = 0;
    float fcb[40];
    for (u32 i = 0; i < Np; i++) {
        const s32 am = cb[pick[best_k]][phase + i * D];
        code = 3 * code + (am == 1 ? 1 : 2 * am + 2);
    }
    shape[s] = static_cast<u32>(code) * D + phase;
    for (u32 k = 0; k < sb; k++) fcb[k] = csflt(cb[pick[best_k]][k]);

    // The excitation into the adaptive codebook and the local decoder's state.
    std::memmove(acb, acb + sb, (295 - sb) * sizeof(float));
    for (u32 n = 0; n < sb; n++) {
        const float exc = fma(again, acb_part[n], fm(fgain, fcb[n]));
        acb[295 - sb + n] = exc;
        const float out = fa(exc, sum20(aw, s_state));
        push(s_state, out);
    }
}

u32 RpeEncoder::encode_frame(const u8* pcm, bool float_input, u8* au)
{
    float x[240];
    for (u32 i = 0; i < cfg.frame_size; i++) {
        if (float_input) {
            const u32 v = (u32{pcm[4 * i]} << 24) | (u32{pcm[4 * i + 1]} << 16) | (u32{pcm[4 * i + 2]} << 8) | pcm[4 * i + 3];
            x[i] = fm(from_bits(v), 32767.0f);
        } else {
            x[i] = csflt(static_cast<s16>(static_cast<u16>(pcm[2 * i] | (pcm[2 * i + 1] << 8))));
        }
    }
    return encode(x, au);
}

namespace {

// MSB-first bits into a zeroed buffer.
struct BitWriter {
    u8* p;
    u32 pos = 0;
    void put(u32 value, u32 n)
    {
        for (u32 i = 0; i < n; i++, pos++)
            if ((value >> (n - 1 - i)) & 1) p[pos >> 3] |= static_cast<u8>(0x80 >> (pos & 7));
    }
};

} // namespace

// The header: Sony's magic (".mp4"), three fields always 0, 0x4003 and 1, the sampling rate, the bit rate, a frame's
// bits twice (from the bit rate, rounded as the SPU program does: in 1/32768 units of a sample period), 32 bits of 0,
// two fields always 2 and 6, then MPEG-4 CELP's CelpHeader (14496-3 subpart 3: ExcitationMode RPE, SampleRateMode
// 16 kHz, FineRateControl off, RPE_Configuration), the last byte padded with zeros.
u32 rpe_header(u32 configuration, u8* out)
{
    static constexpr u32 kBitRates[4] = {14400, 16000, 18667, 22533};
    const RpeConfig& c = kRpeConfigs[configuration];
    constexpr u32 kRate = 16000;
    const s32 period = static_cast<s32>(c.frame_size << 15) / static_cast<s32>(kRate);
    const u32 frame_bits = static_cast<u32>(period * static_cast<s32>(kBitRates[configuration]) + 0x4000) >> 15;
    std::memset(out, 0, 28);
    BitWriter w{out};
    w.put(0x2E6D7034, 32);
    w.put(0, 8);
    w.put(0x4003, 16);
    w.put(1, 8);
    w.put(kRate, 32);
    w.put(kBitRates[configuration], 32);
    w.put(frame_bits, 16);
    w.put(frame_bits, 16);
    w.put(0, 32);
    w.put(2, 8);
    w.put(6, 16);
    w.put(0x30 | configuration, 6); // ExcitationMode 1, SampleRateMode 1, FineRateControl 0, RPE_Configuration
    return (w.pos + 7) >> 3;
}

// The access unit: the LSP indices, then each subframe's lag, shape, adaptive and fixed codebook gain indices, MSB
// first, the last byte padded with zeros.
u32 RpeEncoder::pack(u8* au) const
{
    u32 bitpos = 0;
    const u32 bytes = cfg.frame_bytes;
    std::memset(au, 0, bytes);
    auto put = [&](u32 value, u32 n) {
        for (u32 i = 0; i < n; i++, bitpos++)
            if ((value >> (n - 1 - i)) & 1) au[bitpos >> 3] |= static_cast<u8>(0x80 >> (bitpos & 7));
    };
    static constexpr u32 kLspBits[10] = {5, 5, 7, 7, 1, 4, 4, 7, 5, 1};
    for (u32 k = 0; k < 10; k++) put(lsp_idx[k], kLspBits[k]);
    const u32 shape_bits = (config + 22) >> 1;
    for (u32 s = 0; s < cfg.n_subframes; s++) {
        put(lag[s], 8);
        put(shape[s], shape_bits);
        put(again_idx[s], 6);
        put(fgain_idx[s], s == 0 ? 5 : 3);
    }
    return bytes;
}

void RpeEncoder::reset(u32 configuration)
{
    config = configuration;
    cfg = kRpeConfigs[configuration];
    buffer_len = 3 * cfg.frame_size + cfg.sbfrm_size / 2;
    // The squared Hamming window of the LPC analysis: x / N in single precision, the rest in double (the SPU's libm
    // cosine), rounded to single precision and squared there.
    const u32 n = cfg.window;
    for (u32 x = 0; x < n; x++) {
        const float q = div_gcc(csflt(static_cast<s32>(x)), csflt(static_cast<s32>(n)));
        float w;
        {
            Nearest nearest;
            const double arg = static_cast<double>(q) * 6.283185307179586;
            const double c = std::cos(arg);
            w = frds(c * -0.46 + 0.54);
        }
        window[x] = fm(w, w);
    }
    gamma_be[0] = from_bits(0x3F7D013Bu); // 0.9883
    for (int k = 1; k < 20; k++) gamma_be[k] = fm(gamma_be[k - 1], gamma_be[0]);
    std::memset(buffer, 0, sizeof buffer);
    frame_ctr = 0;
    rfc0 = 0;
    std::memset(lpc, 0, sizeof lpc);
    std::memset(next_lsp, 0, sizeof next_lsp);
    for (int k = 0; k < 20; k++) {
        const float v = fm(fa(csflt(k), 1.0f), from_bits(0x3D430C30u)); // (k + 1) / 21
        prev_q[k] = cur_lsp[k] = v;
    }
    std::memset(q_lsp, 0, sizeof q_lsp);
    std::memset(lsp_idx, 0, sizeof lsp_idx);
    std::memset(ap, 0, sizeof ap);
    std::memset(mem_i, 0, sizeof mem_i);
    std::memset(wden, 0, sizeof wden);
    std::memset(s_state, 0, sizeof s_state);
    std::memset(acb, 0, sizeof acb);
    std::memset(e_tbl, 0, sizeof e_tbl);
    std::memset(rap, 0, sizeof rap);
    gp = 0;
    af_cur = 0;
    afp = 0;
}

// The LPC analysis of the buffer's window: the windowed samples, their autocorrelation (four lanes of four running
// sums, as the SPU program's vector loop), Levinson-Durbin in single precision (the reference software's DEBUG_YM
// variant: it stops where the error falls to a millionth of the energy), the bandwidth expansion.
void RpeEncoder::lpc_analysis()
{
    const u32 N = cfg.window;
    float s[400];
    for (u32 k = 0; k < N; k++) s[k] = fm(buffer[cfg.offset + k], window[k]);
    float acf[21];
    for (u32 i = 0; i <= 20; i++) {
        const u32 count = N - i;
        float acc[4][4] = {}; // [vector][lane]
        u32 k = 0;
        for (; k + 16 <= count; k += 16)
            for (int v = 0; v < 4; v++)
                for (int j = 0; j < 4; j++) {
                    const u32 at = k + 4 * v + j;
                    acc[v][j] = fma(s[at], s[at + i], acc[v][j]);
                }
        float lane[4];
        for (int j = 0; j < 4; j++) lane[j] = fa(fa(fa(acc[0][j], acc[1][j]), acc[2][j]), acc[3][j]);
        for (; k + 4 <= count; k += 4)
            for (int j = 0; j < 4; j++) lane[j] = fma(s[k + j], s[k + j + i], lane[j]);
        if (k < count)
            for (u32 j = 0; j < count - k; j++) lane[j] = fma(s[k + j], s[k + j + i], lane[j]);
        acf[i] = fa(fa(lane[0], lane[2]), fa(lane[1], lane[3]));
    }
    float rfc[20] = {}, apar[20] = {}, tmp[20] = {};
    float E = acf[0];
    if (fcgt(E, 1.0f)) {
        const float thr = fm(E, from_bits(0x358637BDu)); // 1e-6
        for (int i = 0; i < 20; i++) {
            float t = 0;
            for (int j = 0; j < i; j++) t = fma(tmp[j], acf[i - j], t);
            rfc[i] = div_gcc(fs(acf[i + 1], t), E);
            if (!fcgt(1.0f, from_bits(bits(rfc[i]) & 0x7FFFFFFFu)) || !fcgt(E, thr)) {
                for (int j = i; j < 20; j++) rfc[j] = apar[j] = 0;
                break;
            }
            apar[i] = rfc[i];
            for (int j = 0; j < i; j++) apar[j] = fnms(rfc[i], tmp[i - j - 1], tmp[j]);
            E = fm(E, fnms(rfc[i], rfc[i], 1.0f));
            for (int j = 0; j <= i; j++) tmp[j] = apar[j];
        }
    }
    rfc0 = rfc[0];
    for (int k = 0; k < 20; k++) lpc[k] = fm(apar[k], gamma_be[k]);
}

u32 RpeEncoder::encode(const float* pcm, u8* au)
{
    (void)au;
    const u32 F = cfg.frame_size;
    // The input goes into the analysis buffer as it is (Sony's encoder leaves the reference software's DC notch off).
    std::memmove(buffer, buffer + F, (buffer_len - F) * sizeof(float));
    std::memcpy(buffer + buffer_len - F, pcm, F * sizeof(float));
    if (++frame_ctr <= 2) return 0;
    lpc_analysis();
    // The LSPs of this analysis, normalised: they are quantised a frame later (the analysis window runs a frame
    // ahead). Roots pc2lsf does not find keep the last value, scaled again.
    {
        float lsf[20];
        const int nf = lpc_to_lsf(lpc, 20, lsf);
        for (int k = 0; k < nf; k++) next_lsp[k] = lsf[k];
        for (int k = 0; k < 20; k++) next_lsp[k] = fm(next_lsp[k], from_bits(0x3EA2F983u)); // 1/pi
    }
    wb_quantize(cur_lsp, prev_q, q_lsp, lsp_idx);
    // Each subframe's LPC coefficients from the quantised LSPs interpolated (the last subframe's the frame's own).
    const u32 n = cfg.n_subframes;
    constexpr float kPi = 3.14159274f; // 0x40490FDB: the encoder's
    for (u32 s = 1; s < n; s++) {
        const float q = div_gcc(csflt(static_cast<s32>(s)), csflt(static_cast<s32>(n)));
        float lsf[20];
        for (int k = 0; k < 20; k++) lsf[k] = fm(fma(fs(q_lsp[k], prev_q[k]), q, prev_q[k]), kPi);
        lsf_to_lpc20(lsf, ap[s - 1]);
    }
    {
        float lsf[20];
        for (int k = 0; k < 20; k++) lsf[k] = fm(q_lsp[k], kPi);
        lsf_to_lpc20(lsf, ap[n - 1]);
    }
    std::memcpy(prev_q, q_lsp, sizeof prev_q);
    std::memcpy(cur_lsp, next_lsp, sizeof cur_lsp);
    // The excitation, subframe by subframe (the first order fit's parameter weighted in double precision).
    {
        Nearest nearest;
        afp = frds(static_cast<double>(af_cur) * 0.8);
    }
    std::memset(rap, 0, sizeof rap);
    for (u32 s = 0; s < n; s++) excitation(s);
    af_cur = rfc0;
    return pack(au);
}

} // namespace pspocket::hle::celp
