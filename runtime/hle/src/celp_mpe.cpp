// The narrowband CELP coder (8 kHz, multi-pulse excitation) of libcelp8enc and libcelp8dec (D-932), as their SPU
// programs compute it. The algorithms are MPEG-4 Audio CELP's (ISO/IEC 14496-3 subpart 3; the reference software,
// ISO/IEC 14496-5: NEC's nec_exc_mp_config.c, nec_qrms_dec.c, nec_exc_module.c, nec_exc_mp_dec.c, nec_exc_gain_dec.c and
// nec_abs_exc_generation.c, Matsushita's pan_lspdec.c and pan_stab.c, AT&T's att_lsf2pc.c, see celp_tables.cpp); the
// order of each operation, and what differs from the reference software, is the SPU programs'.

#include "celp_spu_fp.hpp" // first: the floating-point pragmas

#include <algorithm>
#include <cstring>

#include "celp_codec.hpp"
#include "celp_tables.hpp"

namespace pspocket::hle::celp {

using namespace spu;

// MPE_Configuration 0 to 27 (ISO/IEC 14496-3 subpart 3): the frame, its subframes and each subframe's pulses.
const MpeConfig kMpeConfigs[28] = {
    {320, 4, 80, 3}, {320, 4, 80, 4}, {320, 4, 80, 5},
    {240, 3, 80, 5}, {240, 3, 80, 6}, {240, 3, 80, 7},
    {160, 2, 80, 6}, {160, 2, 80, 7}, {160, 2, 80, 8}, {160, 2, 80, 9}, {160, 2, 80, 10}, {160, 2, 80, 11},
    {160, 2, 80, 12},
    {160, 4, 40, 4}, {160, 4, 40, 5}, {160, 4, 40, 6}, {160, 4, 40, 7}, {160, 4, 40, 8}, {160, 4, 40, 9},
    {160, 4, 40, 10}, {160, 4, 40, 11}, {160, 4, 40, 12},
    {80, 2, 40, 8}, {80, 2, 40, 9}, {80, 2, 40, 10}, {80, 2, 40, 11}, {80, 2, 40, 12},
    {240, 4, 60, 6},
};

// The configurations Sony's programs take: one each for 3850 to 12200 bits a second.
bool mpe_supported(u32 configuration)
{
    switch (configuration) {
    case 0: case 2: case 6: case 9: case 12: case 15: case 18: case 21: case 24: case 26: return true;
    default: return false;
    }
}

// The multi-pulse positions (nec_pulse_bit, nec_pulse_pos): each pulse's bits, from the subframe's even positions
// paired off until `num` channels remain, and each channel's positions.
static bool pulse_bits(u32 len, u32 num, u32 bit[12])
{
    if (len < 2 * num) return false;
    u32 tbit[41] = {}, nbit[41] = {};
    u32 xnum = len / 2;
    for (u32 i = 0; i < xnum; i++) tbit[i] = 1;
    tbit[xnum] = 0;
    while (xnum > num) {
        tbit[xnum] = 0;
        u32 ynum = xnum;
        for (u32 i = 0, j = 0; i < xnum; j++) {
            if (tbit[i] == tbit[i + 1]) {
                nbit[j] = tbit[i] + 1;
                i += 2;
                ynum--;
                if (ynum <= num) {
                    for (u32 k = j + 1, l = i; k < ynum; k++) nbit[k] = tbit[l++];
                    break;
                }
            } else {
                nbit[j] = tbit[i];
                i++;
            }
        }
        if (xnum == ynum) return false;
        xnum = ynum;
        for (u32 i = 0; i < xnum; i++) tbit[i] = nbit[i];
    }
    for (u32 i = 0; i < num; i++) bit[i] = tbit[i];
    return true;
}

static void pulse_positions(u32 len, u32 num, const u32 bit[12], u8 pos[12][80])
{
    s32 ch_num[40];
    u32 mbit = 31;
    for (u32 i = 0; i < num; i++) mbit = std::min(mbit, bit[i]);
    const u32 num_p = 1u << mbit, step = len / num_p;
    for (u32 i = 0; i < step; i++) ch_num[i] = -1;
    for (u32 i = 0; i < num; i++) {
        const u32 mrg_ch = 1u << (bit[i] - mbit);
        // (long)((float)step / mrg_ch + 0.5): exact for these small integers
        const u32 hop = (2 * step + mrg_ch) / (2 * mrg_ch);
        for (u32 j = 0, k = 0; k < mrg_ch;) {
            if (ch_num[j] == -1) {
                ch_num[j] = static_cast<s32>(i);
                k++;
                j = (j + hop) % step;
            } else {
                j++;
            }
        }
    }
    for (u32 i = 0; i < num; i++) {
        u32 l = 0;
        for (u32 k = 0; k < step; k++)
            if (ch_num[k] == static_cast<s32>(i))
                for (u32 j = 0; j < num_p; j++) pos[i][l++] = static_cast<u8>(k + step * j);
    }
}

bool mpe_pulses(u32 configuration, u32 bit[12], u8 pos[12][80])
{
    const MpeConfig& c = kMpeConfigs[configuration];
    if (!pulse_bits(c.sbfrm_size, c.n_pulses, bit)) return false;
    std::memset(pos, 0, 12 * 80);
    pulse_positions(c.sbfrm_size, c.n_pulses, bit, pos);
    return true;
}

namespace {

// MSB-first bits of an access unit (zeros past its end).
struct BitReader {
    const u8* p;
    u32 size;
    u32 pos = 0;
    u32 get(u32 n)
    {
        u32 v = 0;
        for (u32 i = 0; i < n; i++, pos++) {
            const u32 byte = pos >> 3;
            const u32 bit = byte < size ? (p[byte] >> (7 - (pos & 7))) & 1u : 0u;
            v = (v << 1) | bit;
        }
        return v;
    }
};

constexpr float kGapNb = 2.0f / 256; // the narrowband LSPs' minimum distance (PAN_MINGAP_CELP)

float neg(float x) { return from_bits(bits(x) ^ 0x80000000u); }

// 1 / x: the SDK's reciprocal (frest, fi, one step towards 1.0000001).
float recip_sdk(float x)
{
    const float y = fi(x, frest(x));
    return fma(fnms(x, y, from_bits(0x3F800001u)), y, y);
}

// sqrt x (the SDK's sqrtf4): the reciprocal square root estimate, then one step.
float sqrt_sdk(float x)
{
    const float y = fi(x, frsqest(x));
    const float g = fm(y, x);
    const float h = fm(g, 0.5f);
    return fma(fnms(y, g, from_bits(0x3F800001u)), h, g);
}

// The SDK's logarithm (log10f4 and log2f4): x's mantissa m in [sqrt(1/2), sqrt(2)), z = (m - 1) / (m + 1), a
// polynomial in z^2, the exponent added in two parts (`hi_*`, `lo_*`: the constants of the base).
float log_sdk(float x, u32 hi_m, u32 hi_e, u32 lo_m, u32 lo_e, u32 lo_zt)
{
    const float m0 = from_bits((bits(x) & 0x807FFFFFu) | 0x3F000000u);
    const bool small = fcgt(from_bits(0x3F3504F3u), m0);
    const float m = small ? fa(m0, m0) : m0;
    const s32 e = static_cast<s32>((bits(x) >> 23) & 0xFF) - 126 - (small ? 1 : 0);
    const float b = fs(m, 1.0f);
    const float z = div_sdk(b, fa(m, 1.0f));
    const float z2 = fm(z, z);
    const float p = fma(fma(z2, from_bits(0x3E990214u), from_bits(0x3ECCAE10u)), z2, from_bits(0x3F2AAABFu));
    const float t = fms(p, z2, b);
    const float ef = csflt(e);
    const float zt = fm(fm(z, t), from_bits(lo_zt));
    if (hi_e == 0) {
        // log2: the exponent itself
        const float hi = fma(b, from_bits(hi_m), ef);
        const float lo = fma(b, from_bits(lo_m), zt);
        return fa(hi, lo);
    }
    const float hi = fma(b, from_bits(hi_m), fm(ef, from_bits(hi_e)));
    const float lo = fma(b, from_bits(lo_m), fma(ef, from_bits(lo_e), zt));
    return fa(hi, lo);
}

float log10_sdk(float x) { return log_sdk(x, 0x3EDE5BD8u, 0x3E9A2097u, 0x32A93728u, 0x33A13EF3u, 0x3EDE5BD8u); }
float log2_sdk(float x) { return log_sdk(x, 0x3FB8AA3Au, 0, 0x32A5705Fu, 0, 0x3FB8AA3Bu); }

// 2^x (the SDK's exp2f4, as powf4 uses it): 2^ceil(x) times e^-(ceil(x) - x) from a polynomial.
float exp2_sdk(float x)
{
    const float bias = (bits(x) & 0x80000000u) ? 0.0f : from_bits(0x3F7FFFFFu);
    const s32 k = cflts(fa(x, bias));
    const float f = fs(csflt(k), x);
    const float half = from_bits(static_cast<u32>(k + 126) << 23);
    const float scale = fa(half, half);
    const float t = fm(f, from_bits(0x3F317217u));
    const float p1 = fma(t, from_bits(0xB9142E40u), from_bits(0x3AAE4F6Fu));
    const float p2 = fma(t, from_bits(0xBE2AAA4Fu), from_bits(0x3EFFFFFDu));
    const float t2 = fm(t, t);
    const float p3 = fma(t, p1, from_bits(0xBC08026Du));
    const float p4 = fma(t, p2, -1.0f);
    const float t4 = fm(t2, t2);
    const float p5 = fma(t, p3, from_bits(0x3D2AA0E5u));
    const float p6 = fma(t, p4, 1.0f);
    float r = fm(fma(t4, p5, p6), scale);
    if (fcgt(x, from_bits(0x4300FFFFu))) r = from_bits(0x7FFFFFFFu);
    if (fcgt(-126.0f, x)) r = 0.0f;
    return r;
}

// The lag and its fraction (sixths) of an adaptive codebook index (nec_acb_generation's idx2lag tables).
void lag_of(u32 idx, s32* ip, s32* fp)
{
    const s32 i = static_cast<s32>(idx);
    if (i <= 161) {
        *ip = 17 + 2 * i / 6;
        *fp = (2 * i) % 6;
    } else if (i <= 199) {
        *ip = 71 + 3 * (i - 162) / 6;
        *fp = (3 * (i - 162)) % 6;
    } else if (i <= 254) {
        *ip = 90 + (i - 200);
        *fp = 0;
    } else {
        *ip = 0;
        *fp = 0;
    }
}

} // namespace

// The LPC coefficients of 10 line spectral frequencies (radians): the narrowband programs' lsffir, its two halves'
// filters run side by side, the last sum ((w1 + x1) + x2) - w2; a_k = -x_k / 2.
void lsf_to_lpc10(const float lsf[10], float a[10])
{
    double c[10];
    for (int i = 0; i < 10; i++) c[i] = static_cast<double>(fm(cos_spu(lsf[i]), -2.0f));
    double w1[6][2] = {}, w2[6][2] = {};
    float x[10];
    {
        Nearest nearest;
        // the first output (the impulse): xin = 1
        double xin[2] = {1.0, 1.0};
        for (int i = 0; i < 5; i++) {
            double nx[2];
            for (int l = 0; l < 2; l++) nx[l] = dfma(c[2 * i + l], w1[i][l], w2[i][l]) + xin[l];
            for (int l = 0; l < 2; l++) {
                w2[i][l] = w1[i][l];
                w1[i][l] = xin[l];
                xin[l] = nx[l];
            }
        }
        w1[5][0] = xin[0];
        w1[5][1] = xin[1];
        w2[5][0] = xin[1];
        w2[5][1] = 0.0;
        for (int j = 0; j < 10; j++) {
            double xo[2];
            for (int l = 0; l < 2; l++) {
                xo[l] = dfma(c[l], w1[0][l], w2[0][l]);
                w2[0][l] = w1[0][l];
                w1[0][l] = 0.0;
            }
            for (int i = 1; i < 5; i++) {
                for (int l = 0; l < 2; l++) {
                    const double t = dfma(c[2 * i + l], w1[i][l], w2[i][l]);
                    w2[i][l] = w1[i][l];
                    w1[i][l] = xo[l];
                    xo[l] = xo[l] + t;
                }
            }
            const double p1 = w1[5][0], p2 = w2[5][0];
            x[j] = frds(((p1 + xo[0]) + xo[1]) - p2);
            w1[5][0] = xo[0];
            w1[5][1] = xo[1];
            w2[5][0] = xo[1];
            w2[5][1] = 0.0;
        }
    }
    for (int k = 0; k < 10; k++) a[k] = neg(fm(0.5f, x[k]));
}

// The narrowband LSPs (normalised) of a frame's five indices (pan_lspdec with the 22-bit tables): the two first stage
// vectors plus or minus the second stage's, or (the fifth index set) the average of each with the previous frame's
// LSP plus or minus the predicted second stage's; then stabilised (pan_stab) as the narrowband programs compare.
void nb_lsp_decode(const u32 idx[5], const float prev[10], float out[10])
{
    float t[10];
    for (int d = 0; d < 5; d++) {
        t[d] = static_cast<float>(kLspNb[idx[0] * 5 + d]) / 32768.0f;
        t[5 + d] = static_cast<float>(kLspNb[80 + idx[1] * 5 + d]) / 32768.0f;
    }
    const bool minus1 = idx[2] > 63, minus2 = idx[3] > 31;
    const u32 i1 = idx[2] & 63, i2 = idx[3] & 31;
    if (idx[4] == 0) {
        for (int d = 0; d < 5; d++) {
            const float d1 = static_cast<float>(kDNb[i1 * 5 + d]) / 262144.0f;
            const float d2 = static_cast<float>(kDNb[320 + i2 * 5 + d]) / 262144.0f;
            out[d] = minus1 ? fs(t[d], d1) : fa(t[d], d1);
            out[5 + d] = minus2 ? fs(t[5 + d], d2) : fa(t[5 + d], d2);
        }
    } else {
        for (int d = 0; d < 5; d++) {
            const float r1 = static_cast<float>(kPdNb[i1 * 5 + d]) / 524288.0f;
            const float r2 = static_cast<float>(kPdNb[320 + i2 * 5 + d]) / 524288.0f;
            const float s1 = fa(t[d], prev[d]);
            if (d == 4) {
                const float h = fm(s1, 0.5f);
                out[d] = minus1 ? fs(h, r1) : fa(h, r1);
            } else {
                out[d] = minus1 ? fms(s1, 0.5f, r1) : fma(s1, 0.5f, r1);
            }
            const float s2 = fa(t[5 + d], prev[5 + d]);
            out[5 + d] = minus2 ? fms(s2, 0.5f, r2) : fma(s2, 0.5f, r2);
        }
    }
    // Stabilised: sorted (bubble sort), at least the gap from 0, from each other upwards, at most 1 - gap, then
    // from each other downwards.
    for (int n = 9; n > 0; n--) {
        bool swapped = false;
        for (int j = 0; j < n; j++)
            if (fcgt(out[j], out[j + 1])) {
                std::swap(out[j], out[j + 1]);
                swapped = true;
            }
        if (!swapped) break;
    }
    for (int i = 0; i < 10; i++)
        if (fcgt(kGapNb, out[i])) out[i] = kGapNb;
    for (int i = 0; i < 9; i++) {
        const float v = fa(out[i], kGapNb);
        if (fcgt(v, out[i + 1])) out[i + 1] = v;
    }
    const float top = from_bits(0x3F7E0000u); // 1 - 2/256
    for (int i = 0; i < 10; i++)
        if (fcgt(out[i], top)) out[i] = top;
    for (int i = 9; i > 0; i--) {
        const float v = fs(out[i], kGapNb);
        if (fcgt(out[i - 1], v)) out[i - 1] = v;
    }
}

void MpeDecoder::reset(u32 configuration)
{
    config = configuration;
    cfg = kMpeConfigs[configuration];
    std::memset(bit, 0, sizeof bit);
    mpe_pulses(configuration, bit, pos);
    for (int k = 0; k < 10; k++) {
        Nearest nearest;
        prev_lsp[k] = frds(static_cast<double>(k + 1) / 11.0);
    }
    std::memset(lsp, 0, sizeof lsp);
    std::memset(lpc, 0, sizeof lpc);
    std::memset(exc, 0, sizeof exc);
    std::memset(qxnorm, 0, sizeof qxnorm);
    pqxnorm = 0;
    std::memset(mem, 0, sizeof mem);
    std::memset(syn_mem, 0, sizeof syn_mem);
    mute_count = 0;
}

void MpeDecoder::decode(const u8* au, u32 size, u32* consumed, float* y, u32 mute)
{
    const u32 n = cfg.n_subframes, sb = cfg.sbfrm_size, np = cfg.n_pulses;
    u32 pos_bits = 0;
    for (u32 i = 0; i < np; i++) pos_bits += bit[i];

    // The frame: the LSP indices (22 bits), the signal mode, the RMS index; each subframe's lag, pulse positions,
    // signs and gain index; byte aligned.
    BitReader br{au, size};
    u32 lsp_idx[5];
    lsp_idx[0] = br.get(4);
    lsp_idx[1] = br.get(4);
    lsp_idx[2] = br.get(7);
    lsp_idx[3] = br.get(6);
    lsp_idx[4] = br.get(1);
    const u32 vu = br.get(2), rms_idx = br.get(6);
    u32 lag_idx[4], pos_idx[4], sgn_idx[4], gain_idx[4];
    for (u32 s = 0; s < n; s++) {
        lag_idx[s] = br.get(8);
        pos_idx[s] = br.get(pos_bits);
        sgn_idx[s] = br.get(np);
        gain_idx[s] = br.get(6);
    }
    *consumed = (br.pos + 7) >> 3;

    // The LSPs, each subframe's interpolated from the previous frame's (the last subframe's are the frame's own),
    // into LPC coefficients.
    nb_lsp_decode(lsp_idx, prev_lsp, lsp);
    const float pi = from_bits(0x40490FDBu);
    const float nf = csflt(static_cast<s32>(n));
    for (u32 s = 0; s + 1 < n; s++) {
        const float q = div_sdk(csflt(static_cast<s32>(s + 1)), nf);
        float lsf[10];
        for (int k = 0; k < 10; k++) lsf[k] = fm(fma(fs(lsp[k], prev_lsp[k]), q, prev_lsp[k]), pi);
        lsf_to_lpc10(lsf, lpc[s]);
    }
    std::memcpy(prev_lsp, lsp, sizeof lsp);
    {
        float lsf[10];
        for (int k = 0; k < 10; k++) lsf[k] = fm(lsp[k], pi);
        lsf_to_lpc10(lsf, lpc[n - 1]);
    }

    // The RMS of each subframe (nec_dec_rms): mu-law steps from the previous frame's last subframe's to the frame's.
    const float mu = vu == 0 ? 1024.0f : 512.0f, rms_max = vu == 0 ? 7932.0f : 15864.0f;
    const float bb = div_sdk(rms_max, mu);
    const float L = log10_sdk(fa(mu, 1.0f));
    const float aa = recip_sdk(L);
    const float pwk = fm(aa, log10_sdk(fma(pqxnorm, recip_sdk(bb), 1.0f)));
    const float log2_10 = log2_sdk(10.0f);
    const float d = fms(csflt(static_cast<s32>(rms_idx + 1)), 1.0f / 64, pwk);
    for (u32 s = 0; s < n; s++) {
        const float nwk = fma(d, div_sdk(csflt(static_cast<s32>(s + 1)), nf), pwk);
        qxnorm[s] = fm(bb, fs(exp2_sdk(fm(fm(nwk, L), log2_10)), 1.0f));
    }
    pqxnorm = qxnorm[n - 1];

    // Each subframe's normalised power: its LPC filter's reflection coefficients' prod(1 - k^2) (nec_lpc2par,
    // innermost last) times the subframe's samples times the RMS squared.
    float renorm2[4];
    for (u32 s = 0; s < n; s++) {
        float f[10], b[10];
        for (int i = 0; i < 10; i++) f[i] = neg(lpc[s][i]);
        float p = 1.0f;
        for (int mr = 9; mr >= 0; mr--) {
            const float dd = fnms(f[mr], f[mr], 1.0f);
            p = fm(p, dd);
            const float inv = recip_sdk(dd);
            for (int k = 0; k <= mr; k++) b[k] = f[k];
            for (int k = 0; k < mr; k++) f[k] = fm(fnms(b[mr], b[mr - 1 - k], b[k]), inv);
        }
        renorm2[s] = fm(p, fm(csflt(static_cast<s32>(sb)), fm(qxnorm[s], qxnorm[s])));
    }

    // A muted frame: the largest excitation samples zeroed, more each muted frame in a row.
    u32 zeroes = 0;
    if (mute) {
        const u32 c = mute_count++;
        const u32 step = sb >> 3, cap = (3 * sb) >> 2;
        zeroes = static_cast<u64>(c + 1) * step > cap ? cap : (c + 1) * step;
    } else {
        mute_count = 0;
    }

    const float comb_gain = vu == 2 ? 0.6f : 0.8f;
    const float fil0 = from_bits(0x3F7FFE00u); // 32767/32768: the interpolation filter's middle tap
    for (u32 s = 0; s < n; s++) {
        float* e = exc + s * sb;
        // The adaptive codebook (nec_acb_generation): the past excitation at the lag, repeated each lag's samples,
        // interpolated (10 taps of 11: Sony's programs leave out the last) at a fraction.
        float acb[80] = {};
        s32 ip, fp;
        lag_of(lag_idx[s], &ip, &fp);
        if (ip > 0) {
            if (fp == 0) {
                for (u32 smp = 0; smp < sb;)
                    for (s32 i = 0; i < ip && smp < sb; i++, smp++) {
                        const float v = fm(mem[150 - ip + i], fil0);
                        acb[smp] = v;
                        mem[150 + smp] = v;
                    }
            } else {
                s32 f0 = 0;
                for (u32 smp = 0; smp < sb;) {
                    f0 += fp;
                    const s32 i0 = ip + f0 / 6;
                    f0 %= 6;
                    for (s32 i = 0; i < i0 && smp < sb; i++, smp++) {
                        float acc = 0.0f;
                        for (s32 k = -5; k <= 4; k++) {
                            const s32 kk = (k + 1) * 6 - f0;
                            acc = fma(static_cast<float>(kNbFil[kk < 0 ? -kk : kk]) / 32768.0f, mem[149 - i0 + i - k], acc);
                        }
                        acb[smp] = acc;
                        mem[150 + smp] = acc;
                    }
                }
            }
        }
        // The pulses (nec_dec_mp): each pulse's position index and sign from the indices' low bits up, the last
        // pulse's first; then the comb filter at the lag (voiced frames).
        float comb[80] = {};
        s32 pidx = static_cast<s32>(pos_idx[s]), sidx = static_cast<s32>(sgn_idx[s]);
        for (s32 i = static_cast<s32>(np) - 1; i >= 0; i--) {
            const float sign = (sidx & 1) ? -1.0f : 1.0f;
            sidx >>= 1;
            const u32 loc = static_cast<u32>(pidx) & ((1u << bit[i]) - 1);
            pidx >>= bit[i];
            comb[pos[i][loc]] = sign;
        }
        if (vu > 1 && ip > 0 && static_cast<s32>(sb) > ip)
            for (u32 k = static_cast<u32>(ip); k < sb; k++) comb[k] = fma(comb_gain, comb[k - ip], comb[k]);
        // The gains (nec_dec_gain): the codebook's pair times sqrt(power / each vector's energy).
        float v1 = 0, v2 = 0;
        for (u32 k = 0; k < sb; k++) {
            v1 = fma(acb[k], acb[k], v1);
            v2 = fma(comb[k], comb[k], v2);
        }
        const u32 g = (4 * (sb == 40 ? 1 : 0) + vu) * 128 + 2 * gain_idx[s];
        const float ga = static_cast<float>(kNecGain[g]) / 16384.0f, ge = static_cast<float>(kNecGain[g + 1]) / 16384.0f;
        const float g_ac = zero_class(v1) ? 0.0f : fm(ga, sqrt_sdk(div_sdk(renorm2[s], v1)));
        const float g_ec = zero_class(v2) ? 0.0f : fm(ge, sqrt_sdk(div_sdk(renorm2[s], v2)));
        for (u32 k = 0; k < sb; k++) e[k] = fma(g_ac, acb[k], fm(g_ec, comb[k]));
        for (u32 z = 0; z < zeroes; z++) {
            float best = 0.0f;
            u32 at = 0;
            for (u32 k = 0; k < sb; k++) {
                const float m = from_bits(bits(e[k]) & 0x7FFFFFFFu);
                if (fcgt(m, best)) {
                    best = m;
                    at = k;
                }
            }
            e[at] = 0.0f;
        }
        // The past excitation moves on.
        std::memmove(mem, mem + sb, (150 - sb) * sizeof(float));
        std::memcpy(mem + 150 - sb, e, sb * sizeof(float));
    }

    // The synthesis filter, each subframe's LPC coefficients; the output unclamped.
    for (u32 s = 0; s < n; s++) {
        const float* a = lpc[s];
        for (u32 k = 0; k < sb; k++) {
            float p[4];
            for (int j = 0; j < 4; j++) {
                p[j] = fm(a[j], syn_mem[j]);
                p[j] = fma(a[4 + j], syn_mem[4 + j], p[j]);
                if (j < 2) p[j] = fma(a[8 + j], syn_mem[8 + j], p[j]);
            }
            const float v = fa(exc[s * sb + k], fa(fa(p[0], p[2]), fa(p[1], p[3])));
            std::memmove(syn_mem + 1, syn_mem, 11 * sizeof(float));
            syn_mem[0] = v;
            y[s * sb + k] = v;
        }
    }
}

} // namespace pspocket::hle::celp
