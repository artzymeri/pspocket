// The wideband CELP coder (16 kHz, regular pulse excitation) of libcelpenc and libcelpdec (D-932), as their SPU
// programs compute it. The algorithms are MPEG-4 Audio CELP's (ISO/IEC 14496-3 subpart 3; the reference software,
// ISO/IEC 14496-5: phi_lpc.c, pan_lspdec.c, pan_stab.c, phi_gxit.c and phi_xits.c by Philips Research and Matsushita, see
// celp_tables.cpp); the order of each operation, and what differs from the reference software, is the SPU programs'.

#include "celp_spu_fp.hpp" // first: the floating-point pragmas

#include <algorithm>
#include <cstring>

#include "celp_codec.hpp"
#include "celp_tables.hpp"

namespace pspocket::hle::celp {

using namespace spu;

namespace {

// MSB-first bits of an access unit.
struct BitReader {
    const u8* p;
    u32 size;
    u32 pos = 0; // bits read
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

constexpr float kPi = 3.1415925f; // 0x40490FDA

} // namespace

void RpeDecoder::reset(u32 configuration)
{
    config = configuration;
    cfg = kRpeConfigs[configuration];
    for (int k = 0; k < 20; k++) {
        Nearest nearest;
        prev_lsp[k] = frds(static_cast<double>(k + 1) * (1.0 / 21.0));
    }
    std::memset(acb, 0, sizeof acb);
    std::memset(syn_mem, 0, sizeof syn_mem);
    prev_fcb_gain = 0;
}

bool RpeDecoder::decode(const u8* au, u32 size, u32* consumed, float* y_out, u32 mute)
{
    BitReader br{au, size};
    // The frame: LSP indices (46 bits), then each subframe's lag, shape, adaptive and fixed codebook gain indices.
    u32 lower[5], upper[5];
    lower[0] = br.get(5);
    lower[1] = br.get(5);
    lower[2] = br.get(7);
    lower[3] = br.get(7);
    lower[4] = br.get(1);
    upper[0] = br.get(4);
    upper[1] = br.get(4);
    upper[2] = br.get(7);
    upper[3] = br.get(5);
    upper[4] = br.get(1);
    const u32 shape_bits = (config + 22) >> 1;
    u32 lag[10], shape[10], again_idx[10], fgain_idx[10];
    for (u32 s = 0; s < cfg.n_subframes; s++) {
        lag[s] = br.get(8);
        shape[s] = br.get(shape_bits);
        again_idx[s] = br.get(6);
        fgain_idx[s] = br.get(s == 0 ? 5 : 3);
    }
    *consumed = (br.pos + 7) >> 3;
    if (*consumed > size) return false;

    // The LSPs of the frame, stabilised.
    float cur[20];
    wb_lsp_half(lower, prev_lsp, cur, false);
    wb_lsp_half(upper, prev_lsp + 10, cur + 10, true);
    stabilise_lsp20(cur);

    // Each subframe's LPC coefficients: the LSPs interpolated from the previous frame's (the last subframe's are the
    // frame's own), in radians.
    float ap[10][20];
    const u32 n = cfg.n_subframes;
    const float nf = csflt(static_cast<s32>(n));
    for (u32 s = 1; s < n; s++) {
        const float q = div_sdk(csflt(static_cast<s32>(s)), nf);
        float lsf[20];
        for (int k = 0; k < 20; k++) lsf[k] = fm(kPi, fma(fs(cur[k], prev_lsp[k]), q, prev_lsp[k]));
        lsf_to_lpc20(lsf, ap[s - 1]);
    }
    {
        float lsf[20];
        for (int k = 0; k < 20; k++) lsf[k] = fm(kPi, cur[k]);
        lsf_to_lpc20(lsf, ap[n - 1]);
    }
    std::memcpy(prev_lsp, cur, sizeof cur);

    const u32 sb = cfg.sbfrm_size, D = cfg.D, Np = cfg.Np;
    for (u32 s = 0; s < n; s++) {
        // The regular pulses: the phase, then each pulse's amplitude (-1, 1 or 0) from the index's ternary digits.
        const u32 phase = shape[s] % D;
        s32 ac = static_cast<s32>(shape[s] / D);
        s32 amp[6];
        for (int i = static_cast<int>(Np) - 1; i >= 0; i--) {
            const s32 r = ac % 3;
            ac /= 3;
            amp[i] = r == 0 ? -1 : r == 1 ? 1 : 0;
        }
        // The gains: the adaptive codebook's signed, the fixed codebook's direct in the first subframe and relative
        // to the previous one after.
        const u32 ai = again_idx[s];
        const float again = ai <= 31 ? kCbaGain[ai] : from_bits(bits(kCbaGain[63 - ai]) ^ 0x80000000u);
        const float fgain = s == 0 ? kCbfGain[fgain_idx[s]] : fm(kCbfDiffGain[fgain_idx[s]], prev_fcb_gain);
        prev_fcb_gain = fgain;

        float acb_part[40], fcb[40];
        const u32 start = 255 - lag[s];
        for (u32 k = 0; k < sb; k++) acb_part[k] = acb[start + k];
        for (u32 k = 0; k < sb; k++) fcb[k] = 0;
        for (u32 k = 0; k < Np; k++) fcb[phase + k * D] = csflt(amp[k]);
        std::memmove(acb, acb + sb, (295 - sb) * sizeof(float));

        float exc[40];
        for (u32 k = 0; k < sb; k++) exc[k] = fma(again, acb_part[k], fm(fgain, fcb[k]));
        if (mute) {
            // Sony's decoder told to (an access unit's user data): the excitation's largest three quarters zeroed, one
            // sample at a time, the first of the largest magnitude.
            const u32 times = (3 * sb) >> 2;
            for (u32 t = 0; t < times; t++) {
                u32 at = 0;
                float max = 0;
                for (u32 k = 0; k < sb; k++) {
                    const float m = from_bits(bits(exc[k]) & 0x7FFFFFFFu);
                    if (fcgt(m, max)) {
                        max = m;
                        at = k;
                    }
                }
                exc[at] = 0;
            }
        }
        // The synthesis filter: the 20 products summed four lanes at a time, the lanes then pairwise.
        const float* a = ap[s];
        for (u32 k = 0; k < sb; k++) {
            acb[295 - sb + k] = exc[k];
            float lane[4];
            for (int j = 0; j < 4; j++) {
                float v = fm(a[j], syn_mem[j]);
                for (int g = 1; g < 5; g++) v = fma(a[4 * g + j], syn_mem[4 * g + j], v);
                lane[j] = v;
            }
            const float sum = fa(fa(lane[1], lane[3]), fa(lane[0], lane[2]));
            const float y = fa(exc[k], sum);
            for (int j = 19; j > 0; j--) syn_mem[j] = syn_mem[j - 1];
            syn_mem[0] = y;
            y_out[s * sb + k] = y;
        }
    }
    return true;
}

} // namespace pspocket::hle::celp
