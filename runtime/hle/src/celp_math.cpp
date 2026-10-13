// The CELP libraries' shared arithmetic (D-932): what Sony's SPU programs compute for each of them, in the SPU's
// single precision (celp_spu_fp.hpp). Written from the MPEG-4 Audio reference software's algorithms (ISO/IEC 14496-5,
// the CELP decoder and encoder of ISO/IEC 14496-3 subpart 3: att_lsf2pc.c and phi_lsfr.c's lsffir, Peter Kroon, Bell
// Laboratories; see celp_tables.cpp for the licence those modules carry) as the SPU programs order their operations.

#include "celp_spu_fp.hpp" // first: the floating-point pragmas

#include <utility>

#include "celp_codec.hpp"
#include "celp_tables.hpp"

namespace pspocket::hle::celp {

using namespace spu;

namespace {
constexpr float kGap = 1.0f / 256; // the wideband LSPs' minimum distance (PAN_MINGAP_CELP_W)
} // namespace

// cos x as the SPU programs compute it: the Cell SDK SIMD math library's cosf4 (a quadrant from x * 2/pi rounded half
// away from zero, the remainder in two steps of pi/2, then its sine or cosine polynomial).
float cos_spu(float x)
{
    const float two_over_pi = from_bits(0x3F22F983u); // 0.63661977
    float xl = fm(x, two_over_pi);
    // spu_sel(0.5f, xl, 0x80000000): one half with xl's sign
    xl = fa(xl, from_bits(0x3F000000u | (bits(xl) & 0x80000000u)));
    const s32 q = cflts(xl);
    const s32 offset = 1 + (q & 3);
    const float qf = csflt(q);
    const float p1 = fnms(qf, from_bits(0x3FC90FD9u), x);  // pi/2, high part
    const float r = fnms(qf, from_bits(0x33A22168u), p1);  // pi/2, low part
    const float r2 = fm(r, r);
    const float r3 = fm(r2, r);
    const float ct1 = fma(r2, from_bits(0xBAB24992u), from_bits(0x3D2AA035u));
    const float st1 = fma(r2, from_bits(0xB94C8C6Du), from_bits(0x3C088342u));
    const float ct2 = fma(ct1, r2, from_bits(0xBEFFFFDFu));
    const float st2 = fma(st1, r2, from_bits(0xBE2AAAA1u));
    const float cx = fma(ct2, r2, 1.0f);
    const float sx = fma(st2, r3, r);
    float res = (offset & 1) == 0 ? sx : cx;
    if ((offset & 2) != 0) res = from_bits(bits(res) ^ 0x80000000u);
    return res;
}

// The LPC coefficients of 20 line spectral frequencies (radians): the impulse response of the LSF filter (lsffir) in
// double precision, from -2 cos w in single precision; a_k = -x_k / 2 (the decoder's and encoder's predictor sign).
void lsf_to_lpc20(const float lsf[20], float a[20])
{
    double c[20];
    for (int i = 0; i < 20; i++) c[i] = static_cast<double>(fm(cos_spu(lsf[i]), -2.0f));
    double w[2 * 20 + 2] = {};
    float x[21];
    {
        Nearest nearest;
        for (int j = 0; j < 21; j++) {
            double xin1 = j == 0 ? 1.0 : 0.0, xin2 = xin1;
            int n4 = 0;
            for (int i = 0; i < 10; i++) {
                const int n1 = i * 4, n2 = n1 + 1, n3 = n2 + 1;
                n4 = n3 + 1;
                const double xout1 = dfma(c[2 * i], w[n1], w[n2]) + xin1;
                const double xout2 = dfma(c[2 * i + 1], w[n3], w[n4]) + xin2;
                w[n2] = w[n1];
                w[n1] = xin1;
                w[n4] = w[n3];
                w[n3] = xin2;
                xin1 = xout1;
                xin2 = xout2;
            }
            const double xout1 = xin1 + w[n4 + 1];
            const double xout2 = xin2 - w[n4 + 2];
            x[j] = frds(xout1 + xout2);
            w[n4 + 1] = xin1;
            w[n4 + 2] = xin2;
        }
    }
    for (int k = 0; k < 20; k++) a[k] = from_bits(bits(fm(x[k + 1], 0.5f)) ^ 0x80000000u);
}

// acos x as the SPU programs compute it: the Cell SDK SIMD math library's acosf4 (a polynomial in |x| in two halves,
// times sqrt(1 - |x|) from the reciprocal square root estimate and one step; pi less it for negative x).
float acos_spu(float x)
{
    const float xabs = from_bits(bits(x) & 0x7FFFFFFFu);
    const float one_minus = fs(1.0f, xabs);
    const float y0 = fi(one_minus, frsqest(one_minus));
    const float y0x = fm(y0, one_minus);
    const float y0xhalf = fm(y0x, 0.5f);
    const float t1 = fma(fnms(y0, y0x, from_bits(0x3F800001u)), y0xhalf, y0x);
    const float xabs2 = fm(xabs, xabs);
    const float xabs4 = fm(xabs2, xabs2);
    float hi = fma(xabs, from_bits(0xBAA57A2Cu), from_bits(0x3BDA90C5u));
    float lo = fma(xabs, from_bits(0xBD4D8392u), from_bits(0x3DB63A9Eu));
    lo = fma(lo, xabs, from_bits(0xBE5BBFCAu));
    hi = fma(hi, xabs, from_bits(0xBC8BFC66u));
    lo = fma(lo, xabs, from_bits(0x3FC90FDAu));
    hi = fma(hi, xabs, from_bits(0x3CFD10F8u));
    const float result = fma(hi, xabs4, lo);
    return (bits(x) & 0x80000000u) ? fnms(t1, result, from_bits(0x40490FDAu)) : fm(t1, result);
}

namespace {

// A Chebyshev series at x (FNevChebP): the backward recursion b = 2x b1 - b2 + c(i), the SPU program's way.
float cheb(float x, const float* c, int n)
{
    const float x2 = fm(x, 2.0f);
    float b0 = 0, b1 = 0, b2 = 0;
    for (int i = n - 1; i >= 0; i--) {
        b2 = b1;
        b1 = b0;
        b0 = fma(b1, x2, fs(c[i], b2));
    }
    return fm(fa(fs(b0, b2), c[0]), 0.5f);
}

} // namespace

// The line spectral frequencies (radians) of an even order LPC polynomial 1 - a1 z^-1 - ...: P. Kabal's pc2lsf
// (att_pc2lsf.c) as the SPU programs run it: the roots of the sum and difference polynomials' Chebyshev series, sampled
// in steps of about pi/100, four bisections, a linear interpolation, acos. The wideband program starts the next search
// where the bisection ended (the reference software at the interpolated root) and interpolates with GCC's division;
// the narrowband one (`nb`) as the reference software, with the SDK's division. Returns the roots found.
int lpc_to_lsf(const float* a, int np, float* lsf, bool nb)
{
    const int nh = np / 2 + 1;
    float fa_[11], fb_[11], ta[11], tb[11];
    fa_[0] = 1.0f;
    fb_[0] = 1.0f;
    for (int i = 1, j = np; i < nh; ++i, --j) {
        fa_[i] = from_bits(bits(fa(a[i - 1], a[j - 1])) ^ 0x80000000u);
        fb_[i] = from_bits(bits(fs(a[i - 1], a[j - 1])) ^ 0x80000000u);
    }
    for (int i = 1; i < nh; ++i) {
        fa_[i] = fs(fa_[i], fa_[i - 1]);
        fb_[i] = fa(fb_[i], fb_[i - 1]);
    }
    ta[0] = fa_[nh - 1];
    tb[0] = fb_[nh - 1];
    for (int i = 1, j = nh - 2; i < nh; ++i, --j) {
        ta[i] = fm(2.0f, fa_[j]);
        tb[i] = fm(2.0f, fb_[j]);
    }
    const float aa = from_bits(0xBCF125DDu), ss = from_bits(0x3D00A891u); // 4 - 4 cos(pi/100) - sin(pi/100), sin
    int nf = 0;
    const float* t = ta;
    float xroot = 2.0f, xlow = 1.0f;
    float ylow = cheb(xlow, t, nh);
    while (fcgt(xlow, -1.0f) && nf < np) {
        const float xhigh = xlow;
        float yhigh = ylow;
        const float dx = fma(fm(xhigh, aa), xhigh, ss);
        xlow = fs(xhigh, dx);
        if (fcgt(-1.0f, xlow)) xlow = -1.0f;
        ylow = cheb(xlow, t, nh);
        if (fcgt(fm(ylow, yhigh), 0.0f)) continue;
        float d = fs(xhigh, xlow);
        for (int i = 0; i < 4; i++) {
            d = fm(d, 0.5f);
            const float xmid = fa(xlow, d);
            const float ymid = cheb(xmid, t, nh);
            if (!fcgt(fm(ylow, ymid), 0.0f)) {
                yhigh = ymid;
            } else {
                ylow = ymid;
                xlow = xmid;
            }
        }
        float ratio = 1.0f;
        const bool equal = bits(ylow) == bits(yhigh) || (zero_class(ylow) && zero_class(yhigh));
        if (!equal) {
            const float den = fs(ylow, yhigh);
            if (nb) {
                ratio = div_sdk(ylow, den);
            } else {
                const float y = fi(den, frest(den));
                const float q = fm(ylow, y);
                ratio = fma(fnms(q, den, ylow), y, q);
            }
        }
        const float xmid = nb && equal ? fa(xlow, d) : fma(ratio, d, xlow);
        lsf[nf++] = acos_spu(xmid);
        const float next = nb ? (fcgt(xroot, xmid) ? xmid : fs(xlow, d)) : fs(xlow, fcgt(xroot, xmid) ? 0.0f : d);
        xroot = next;
        t = t == ta ? tb : ta;
        xlow = next;
        ylow = cheb(xlow, t, nh);
    }
    return nf;
}


// The LSPs (normalised to [0, 1)) of one half of the wideband vector: pan_lspdec, the SPU program's way (both stages
// of single precision sums; with prediction (out + previous) / 2 then the prediction table's vector).
void wb_lsp_half(const u32 idx[5], const float prev[10], float out[10], bool upper)
{
    const s16* tbl = upper ? kLspWU : kLspWL;
    const s16* d_tbl = upper ? kDWU : kDWL;
    const s16* pd_tbl = upper ? kPdWU : kPdWL;
    const u32 ncd1 = upper ? 16 : 32, ncd2a = upper ? 16 : 64;
    constexpr float kTbl = 1.0f / 32768, kD = 1.0f / 524288, kPd = 1.0f / 1048576;
    for (u32 i = 0; i < 5; i++) out[i] = tbl[5 * idx[0] + i] * kTbl;
    for (u32 i = 0; i < 5; i++) out[5 + i] = tbl[5 * ncd1 + 5 * idx[1] + i] * kTbl;
    const bool neg0 = idx[2] >= 64, neg1 = idx[3] >= ncd2a;
    const u32 r0 = neg0 ? idx[2] - 64 : idx[2], r1 = neg1 ? idx[3] - ncd2a : idx[3];
    if (idx[4] == 0) {
        for (u32 i = 0; i < 5; i++) {
            const float d = d_tbl[5 * r0 + i] * kD;
            out[i] = neg0 ? fs(out[i], d) : fa(out[i], d);
        }
        for (u32 i = 0; i < 5; i++) {
            const float d = d_tbl[5 * 64 + 5 * r1 + i] * kD;
            out[5 + i] = neg1 ? fs(out[5 + i], d) : fa(out[5 + i], d);
        }
    } else {
        for (u32 i = 0; i < 5; i++) {
            const float d = pd_tbl[5 * r0 + i] * kPd;
            const float t = fa(out[i], prev[i]);
            out[i] = neg0 ? fms(t, 0.5f, d) : fma(t, 0.5f, d);
        }
        for (u32 i = 0; i < 5; i++) {
            const float d = pd_tbl[5 * 64 + 5 * r1 + i] * kPd;
            const float t = fa(out[5 + i], prev[5 + i]);
            out[5 + i] = neg1 ? fms(t, 0.5f, d) : fma(t, 0.5f, d);
        }
    }
}

// pan_stab, the SPU program's way: a bubble sort, then the minimum gap kept upward and downward.
void stabilise_lsp20(float lsp[20])
{
    for (int pass = 19; pass > 0; pass--) {
        bool sorted = true;
        for (int i = 0; i < pass; i++)
            if (fcgt(lsp[i], lsp[i + 1])) {
                std::swap(lsp[i], lsp[i + 1]);
                sorted = false;
            }
        if (sorted) break;
    }
    for (int i = 0; i < 20; i++)
        if (fcgt(kGap, lsp[i])) lsp[i] = kGap;
    for (int i = 0; i < 19; i++) {
        const float t = fa(lsp[i], kGap);
        if (fcgt(t, lsp[i + 1])) lsp[i + 1] = t;
    }
    for (int i = 0; i < 20; i++)
        if (fcgt(lsp[i], 1.0f - kGap)) lsp[i] = 1.0f - kGap;
    for (int i = 18; i >= 0; i--) {
        const float t = fs(lsp[i + 1], kGap);
        if (fcgt(lsp[i], t)) lsp[i] = t;
    }
}

namespace {

// pan_lspqtz2_dd for one half of the wideband vector (two parts of five), Sony's way: the four best first-stage
// vectors of each part (a distance summing the second dimension's term first), then for each of the four pairs both
// second stages, plain (DVQ: the input less the first stage) and predicted (PVQ: the input less the mean of the first
// stage and the previous quantised vector), each table vector added and subtracted, the distances in dimension order;
// each part's best pair separately, then whichever of DVQ and PVQ is nearer, PVQ on a tie.
void quantize_half(const float in[10], const float prev[10], const float w[10], bool upper, u32 idx[5])
{
    const s16* tbl = upper ? kLspWU : kLspWL;
    const s16* d_tbl = upper ? kDWU : kDWL;
    const s16* pd_tbl = upper ? kPdWU : kPdWL;
    const u32 ncd1 = upper ? 16 : 32;
    const u32 ncd2[2] = {64, upper ? 16u : 64u};
    constexpr float kTbl = 1.0f / 32768, kD = 1.0f / 524288, kPd = 1.0f / 1048576;
    // First stage: each part's four nearest rows, nearest first.
    u32 code[2][4];
    for (u32 p = 0; p < 2; p++) {
        float dmin[4];
        for (u32 j = 0; j < 4; j++) {
            dmin[j] = from_bits(0x4E6E6B28u); // 1e9
            code[p][j] = 255;
        }
        const float* x = in + 5 * p;
        const float* wt = w + 5 * p;
        for (u32 r = 0; r < ncd1; r++) {
            const s16* t = tbl + 5 * (ncd1 * p + r);
            float d2[5];
            for (u32 k = 0; k < 5; k++) {
                const float d = fs(x[k], t[k] * kTbl);
                d2[k] = fm(d, d);
            }
            const float dist = fma(d2[4], wt[4], fma(d2[3], wt[3], fma(d2[2], wt[2], fma(d2[0], wt[0], fm(d2[1], wt[1])))));
            for (u32 j = 0; j < 4; j++)
                if (fcgt(dmin[j], dist)) {
                    for (u32 k = 3; k > j; k--) {
                        dmin[k] = dmin[k - 1];
                        code[p][k] = code[p][k - 1];
                    }
                    dmin[j] = dist;
                    code[p][j] = r;
                    break;
                }
        }
    }
    // Second stage for each pair of first-stage vectors.
    float dvq[4][2], pvq[4][2];
    u32 dvq_code[4][2], pvq_code[4][2];
    for (u32 j = 0; j < 4; j++) {
        float c[10], q[10];
        for (u32 k = 0; k < 10; k++) {
            const u32 p = k / 5;
            const float v = tbl[5 * (ncd1 * p + code[p][j]) + k % 5] * kTbl;
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
            for (u32 r = 0; r < ncd2[p]; r++) {
                const s16* t = d_tbl + 5 * (64 * p + r);
                const s16* u = pd_tbl + 5 * (64 * p + r);
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
                dvq_code[j][p] = at_dm + ncd2[p];
            } else {
                dvq[j][p] = min_dp;
                dvq_code[j][p] = at_dp;
            }
            if (fcgt(min_pp, min_pm)) {
                pvq[j][p] = min_pm;
                pvq_code[j][p] = at_pm + ncd2[p];
            } else {
                pvq[j][p] = min_pp;
                pvq_code[j][p] = at_pp;
            }
        }
    }
    // Each part's best pair, for each kind.
    u32 best_d[2], best_p[2];
    for (u32 p = 0; p < 2; p++) {
        best_d[p] = best_p[p] = 0;
        for (u32 j = 1; j < 4; j++) {
            if (fcgt(dvq[best_d[p]][p], dvq[j][p])) best_d[p] = j;
            if (fcgt(pvq[best_p[p]][p], pvq[j][p])) best_p[p] = j;
        }
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

// The wideband LSP quantiser (mod_wb_celp_lsp_quantizer): the weights from the LSPs' distances (each at least the
// minimum gap, their reciprocals from the estimate and one step), each half's indices, the quantised vector decoded
// from them and stabilised.
void wb_quantize(const float in[20], const float prev[20], float q[20], u32 idx[10])
{
    float d[21], r[21], w[20];
    d[0] = in[0];
    for (int i = 1; i < 20; i++) d[i] = fs(in[i], in[i - 1]);
    d[20] = fs(1.0f, in[19]);
    for (int i = 0; i <= 20; i++) {
        const float x = fcgt(d[i], kGap) ? d[i] : kGap;
        const float y = fi(x, frest(x));
        r[i] = fma(fnms(x, y, 1.0f), y, y);
    }
    for (int i = 0; i < 20; i++) w[i] = fa(r[i], r[i + 1]);
    quantize_half(in, prev, w, false, idx);
    quantize_half(in + 10, prev + 10, w + 10, true, idx + 5);
    wb_lsp_half(idx, prev, q, false);
    wb_lsp_half(idx + 5, prev + 10, q + 10, true);
    stabilise_lsp20(q);
}

// a / b as the SPU programs divide (the SDK's divf4: the reciprocal estimate, one step, 1 + 2^-23 for one).
float div_sdk(float a, float b)
{
    const float y = fi(b, frest(b));
    const float t = fnms(b, y, from_bits(0x3F800001u));
    const float ay = fm(a, y);
    return fma(t, ay, ay);
}

// a / b as GCC compiles a single precision division for the SPU (frest, fi, then one correction step).
float div_gcc(float a, float b)
{
    const float y = fi(b, frest(b));
    const float q = fm(a, y);
    const float r = fnms(q, b, a);
    return fma(r, y, q);
}

} // namespace pspocket::hle::celp
