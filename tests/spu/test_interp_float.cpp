// Hand-checked interpreter vectors for the SPU floating-point model (pspocket/spu/fp.h): single precision rounds
// toward zero with denormals flushed (and the RPCS3 "approximate" fixups by default, extended range in the accurate
// model), double precision rounds to nearest. Every expected value is worked out by hand in the comments.

#include <catch_amalgamated.hpp>

#include "pspocket/recomp/spu/asm.hpp"
#include "spu_test_util.hpp"

using namespace pspocket;
using namespace pspocket::spu;
using namespace pspocket::spu::test;
namespace as = pspocket::recomp::spu::as;

namespace {

constexpr u32 kOne = 0x3F800000, kTwo = 0x40000000, kThree = 0x40400000, kHalf = 0x3F000000;
constexpr u32 kMinusOne = 0xBF800000, kNegZero = 0x80000000, kFltMax = 0x7F7FFFFF, kInfPat = 0x7F800000;

u32 run_rr(Machine& m, u32 insn, u32 a, u32 b)
{
    m.r(1) = splat(a);
    m.r(2) = splat(b);
    m.exec(insn);
    return gw(m.r(3), 0);
}

u32 run_rrr(Machine& m, u32 insn, u32 a, u32 b, u32 c)
{
    m.r(1) = splat(a);
    m.r(2) = splat(b);
    m.r(4) = splat(c);
    m.exec(insn);
    return gw(m.r(3), 0);
}

} // namespace

TEST_CASE("single precision, approximate model: round toward zero, flush to zero, saturate")
{
    Machine m;
    const u32 fa = as::fa(3, 1, 2), fs = as::fs(3, 1, 2), fm = as::fm(3, 1, 2);
    CHECK(run_rr(m, fa, kOne, kTwo) == kThree);
    // 1 + 1.5 * 2^-24: round-to-nearest would give 0x3F800001, toward zero keeps 1.0
    CHECK(run_rr(m, fa, kOne, 0x33C00000) == kOne);
    CHECK(run_rr(m, fa, kMinusOne, 0xB3C00000) == kMinusOne);
    // 1 - 1.25 * 2^-24 lies between 1 - 2^-23 (..FE) and 1 - 2^-24 (..FF): nearest is FF, toward zero FE
    CHECK(run_rr(m, fs, kOne, 0x33A00000) == 0x3F7FFFFEu);
    // denormal operands are zero; denormal results flush to zero
    CHECK(run_rr(m, fa, 0x00000005, kOne) == kOne);
    CHECK(run_rr(m, fa, 0x00000005, 0x00000005) == 0u);
    CHECK(run_rr(m, fs, 0x00C00000, 0x00800000) == 0u);
    // overflow rounds toward zero: FLT_MAX, not infinity
    CHECK(run_rr(m, fm, kFltMax, kTwo) == kFltMax);
    CHECK(run_rr(m, fa, kFltMax, kFltMax) == kFltMax);
    CHECK(run_rr(m, fm, 0xFF7FFFFF, kTwo) == 0xFF7FFFFFu);
    // fm: a zero or denormal operand gives +0, even against an exponent-255 operand or a negative one
    CHECK(run_rr(m, fm, kInfPat, 0) == 0u);
    CHECK(run_rr(m, fm, kMinusOne, 0) == 0u);
    CHECK(run_rr(m, fm, 0x00000001, kInfPat) == 0u);
    CHECK(run_rr(m, fm, 0x3FC00000, kTwo) == kThree); // 1.5 * 2
    // fs clamps an exponent-255 subtrahend to FLT_MAX (RPCS3 #4478): "inf - inf" stays the exponent-255 value
    CHECK(run_rr(m, fs, kInfPat, kInfPat) == kInfPat);

    const u32 fma = as::fma(3, 1, 2, 4), fnms = as::fnms(3, 1, 2, 4), fms = as::fms(3, 1, 2, 4);
    CHECK(run_rrr(m, fma, kTwo, kThree, kOne) == 0x40E00000u); // 7
    // fused: (1 + 2^-12)^2 - 1 = 2^-11 + 2^-24 exactly (0x3A000400); a rounded product would give 2^-11
    CHECK(run_rrr(m, fma, 0x3F800800, 0x3F800800, kMinusOne) == 0x3A000400u);
    // a zero multiplicand returns the addend unchanged (bits included)
    CHECK(run_rrr(m, fma, 0, kInfPat, 0x12345678) == 0x12345678u);
    CHECK(run_rrr(m, fnms, kTwo, kThree, 0x41200000) == 0x40800000u); // 10 - 6 = 4
    CHECK(run_rrr(m, fnms, kInfPat, kOne, 0) == 0xFF7FFFFFu);         // 0 - clamp(inf) * 1
    CHECK(run_rrr(m, fms, kTwo, kThree, kOne) == 0x40A00000u);        // 6 - 1 = 5
}

TEST_CASE("single precision, accurate model: extended range")
{
    Machine m;
    m.ctx.xfloat = PS_SPU_XFLOAT_ACCURATE;
    const u32 fa = as::fa(3, 1, 2), fs = as::fs(3, 1, 2), fm = as::fm(3, 1, 2);
    // exponent 255 is a normal exponent: 2^128 + 2^128 = 2^129 saturates to the largest SPU value
    CHECK(run_rr(m, fa, kInfPat, kInfPat) == 0x7FFFFFFFu);
    CHECK(run_rr(m, fm, kInfPat, kHalf) == 0x7F000000u); // 2^128 * 0.5 = 2^127
    CHECK(run_rr(m, fa, kInfPat, 0xFF800000) == 0u);
    CHECK(run_rr(m, fm, kOne, 0x00800000) == 0x00800000u); // 2^-126 is representable
    CHECK(run_rr(m, fm, kHalf, 0x00800000) == 0u);         // 2^-127 underflows to +0
    CHECK(run_rr(m, fs, kMinusOne, kMinusOne) == 0u);      // zero results are +0
    CHECK(run_rr(m, fs, kOne, 0x33A00000) == 0x3F7FFFFEu); // still toward zero
    CHECK(run_rr(m, fa, kFltMax, kFltMax) == 0x7FFFFFFFu); // (2 - 2^-23) * 2^128: exactly the largest value
    CHECK(run_rrr(m, as::fma(3, 1, 2, 4), kInfPat, kTwo, 0xFF800000) == kInfPat); // 2^129 - 2^128
    m.r(1) = splat(0x7FFFFFFF);
    m.exec(as::csflt(3, 1, 255)); // 0x7FFFFFFF * 2^100 saturates
    CHECK(gw(m.r(3), 0) == 0x7FFFFFFFu);
}

TEST_CASE("single-precision compares (both models)")
{
    for (u8 model : {u8{PS_SPU_XFLOAT_APPROXIMATE}, u8{PS_SPU_XFLOAT_ACCURATE}}) {
        Machine m;
        m.ctx.xfloat = model;
        const u32 fcgt = as::fcgt(3, 1, 2), fceq = as::fceq(3, 1, 2), fcmgt = as::fcmgt(3, 1, 2), fcmeq = as::fcmeq(3, 1, 2);
        CHECK(run_rr(m, fcgt, 0x00000005, 0) == 0u); // denormal == zero
        CHECK(run_rr(m, fcgt, kOne, kNegZero) == ~0u);
        CHECK(run_rr(m, fcgt, kNegZero, kMinusOne) == ~0u);
        CHECK(run_rr(m, fcgt, 0x7F800001, kFltMax) == ~0u); // exponent 255 is the largest
        CHECK(run_rr(m, fcgt, 0xC0000000, kMinusOne) == 0u);
        CHECK(run_rr(m, fcgt, 0xFF800000, 0xFF7FFFFF) == 0u);
        CHECK(run_rr(m, fceq, 0, kNegZero) == ~0u);
        CHECK(run_rr(m, fceq, 0x00000001, 0) == ~0u);
        CHECK(run_rr(m, fceq, 0x7FC00000, 0x7FC00000) == ~0u);
        CHECK(run_rr(m, fceq, kOne, kTwo) == 0u);
        CHECK(run_rr(m, fcmgt, 0xC0000000, kOne) == ~0u);
        CHECK(run_rr(m, fcmgt, 0x80000001, 0) == 0u);
        CHECK(run_rr(m, fcmgt, kOne, kMinusOne) == 0u);
        CHECK(run_rr(m, fcmeq, kMinusOne, kOne) == ~0u);
        CHECK(run_rr(m, fcmeq, 0x80000001, 0x00000002) == ~0u);
    }
}

TEST_CASE("integer <-> float conversions: scale, saturation, rounding toward zero")
{
    Machine m;
    const auto conv = [&](u32 insn, u32 a) {
        m.r(1) = splat(a);
        m.exec(insn);
        return gw(m.r(3), 0);
    };
    // csflt rt, ra, I8: rt = ra / 2^(155 - I8)
    CHECK(conv(as::csflt(3, 1, 155), 1) == kOne);
    CHECK(conv(as::csflt(3, 1, 155), static_cast<u32>(-3)) == 0xC0400000u);
    CHECK(conv(as::csflt(3, 1, 154), 1) == kHalf);
    CHECK(conv(as::csflt(3, 1, 156), 1) == kTwo);
    CHECK(conv(as::csflt(3, 1, 155), 0x80000000u) == 0xCF000000u);
    // 16777219 -> toward zero 16777218 (nearest-even would be 16777220)
    CHECK(conv(as::csflt(3, 1, 155), 0x01000003) == 0x4B800001u);
    CHECK(conv(as::cuflt(3, 1, 155), 0xFFFFFFFF) == 0x4F7FFFFFu);
    CHECK(conv(as::csflt(3, 1, 0), 1) == 0u);                 // 2^-155 underflows
    CHECK(conv(as::csflt(3, 1, 0), 0x40000000) == 0x01000000u); // 2^30 * 2^-155 = 2^-125: exact scaling

    // cflts rt, ra, I8: rt = ra * 2^(173 - I8), truncated, saturated
    CHECK(conv(as::cflts(3, 1, 173), 0x40200000) == 2u);                    // 2.5
    CHECK(conv(as::cflts(3, 1, 173), 0xC0200000) == static_cast<u32>(-2)); // -2.5
    CHECK(conv(as::cflts(3, 1, 173), 0x4F32D05E) == 0x7FFFFFFFu);           // 3e9
    CHECK(conv(as::cflts(3, 1, 173), 0xCF32D05E) == 0x80000000u);
    CHECK(conv(as::cflts(3, 1, 172), kOne) == 2u);
    CHECK(conv(as::cflts(3, 1, 173), 0x7FC00000) == 0x7FFFFFFFu); // exponent 255 saturates by sign
    CHECK(conv(as::cflts(3, 1, 173), 0xFFC00000) == 0x80000000u);
    CHECK(conv(as::cflts(3, 1, 173), 0x00000005) == 0u);
    CHECK(conv(as::cfltu(3, 1, 173), kMinusOne) == 0u);
    CHECK(conv(as::cfltu(3, 1, 173), 0x4F9502F9) == 0xFFFFFFFFu); // 5e9
    CHECK(conv(as::cfltu(3, 1, 173), 0x4079999A) == 3u);          // 3.9
    CHECK(conv(as::cfltu(3, 1, 173), kInfPat) == 0xFFFFFFFFu);
    CHECK(conv(as::cfltu(3, 1, 173), 0x4F000000) == 0x80000000u); // 2^31
}

TEST_CASE("frest, frsqest, fi: RPCS3 tables and fixed-point interpolation")
{
    // RPCS3 SPUThread.cpp exponent tables, first and last rows (ps_spu_*_exponent_lut are generated in closed form)
    CHECK(ps_spu_frest_exponent_lut[0] == 0x7F800000u);
    CHECK(ps_spu_frest_exponent_lut[1] == 0x7E000000u);
    CHECK(ps_spu_frest_exponent_lut[127] == 0x3F000000u);
    CHECK(ps_spu_frest_exponent_lut[252] == 0x00800000u);
    CHECK(ps_spu_frest_exponent_lut[253] == 0u);
    CHECK(ps_spu_frest_exponent_lut[255] == 0u);
    CHECK(ps_spu_frsqest_exponent_lut[0] == 0x7F800000u);
    CHECK(ps_spu_frsqest_exponent_lut[1] == 0x5E800000u);
    CHECK(ps_spu_frsqest_exponent_lut[2] == 0x5E800000u);
    CHECK(ps_spu_frsqest_exponent_lut[3] == 0x5E000000u);
    CHECK(ps_spu_frsqest_exponent_lut[255] == 0x1F000000u);

    Machine m;
    const auto one = [&](u32 insn, u32 a) {
        m.r(1) = splat(a);
        m.exec(insn);
        return gw(m.r(3), 0);
    };
    CHECK(one(as::frest(3, 1), kOne) == 0x3F7FFBE0u);      // fraction lut[0] | (253-127) << 23
    CHECK(one(as::frest(3, 1), 0xC0000000) == 0xBEFFFBE0u); // -2: sign kept
    CHECK(one(as::frest(3, 1), 0) == 0x7FFFFBE0u);
    CHECK(one(as::frsqest(3, 1), kOne) == 0x3F7FFDF4u);       // fraction lut[32], exponent 190 - 64
    CHECK(one(as::frsqest(3, 1), 0x40800000) == 0x3EFFFDF4u); // 4.0
    CHECK(one(as::frsqest(3, 1), 0xC0800000) == 0x3EFFFDF4u); // sign ignored

    // fi(a, b): base = (b & 0x7FFC00) << 9, step*y = (b & 0x3FF) * (a & 0x7FFFF)
    CHECK(run_rr(m, as::fi(3, 1, 2), kOne, 0x3F7FFBE0) == 0x3F7FF800u);
    CHECK(run_rr(m, as::fi(3, 1, 2), 0x3F80FFFF, 0x3F7FFBE0) == 0x3F7E0801u);
}

TEST_CASE("double precision: round to nearest, fused multiply-add")
{
    Machine m;
    const auto dd = [&](u32 insn, u64 a, u64 b, u64 t = 0) {
        m.r(1) = dwords(a, a);
        m.r(2) = dwords(b, b);
        m.r(3) = dwords(t, t);
        m.exec(insn);
        CHECK(gd(m.r(3), 0) == gd(m.r(3), 1));
        return gd(m.r(3), 0);
    };
    const u64 one = dbits(1.0), two = dbits(2.0), three = dbits(3.0);
    // 1 + (2^-53 + 2^-60): above the midpoint -> 1 + 2^-52 (toward zero would keep 1.0)
    CHECK(dd(as::dfa(3, 1, 2), one, 0x3CA0200000000000ull) == 0x3FF0000000000001ull);
    CHECK(dd(as::dfa(3, 1, 2), one, 0x3CA0000000000000ull) == one); // tie -> even
    // (1 + 2^-26)(1 + 3*2^-27) = 1 + 5*2^-27 + 1.5 ulp -> tie to even 0x..A000002 (toward zero: ..A000001)
    CHECK(dd(as::dfm(3, 1, 2), 0x3FF0000004000000ull, 0x3FF0000006000000ull) == 0x3FF000000A000002ull);
    CHECK(dd(as::dfs(3, 1, 2), three, one) == two);
    // fused: (1 + 2^-30)^2 - 1 = 2^-29 + 2^-60 exactly
    CHECK(dd(as::dfma(3, 1, 2), 0x3FF0000000400000ull, 0x3FF0000000400000ull, dbits(-1.0)) == 0x3E20000000200000ull);
    CHECK(dd(as::dfms(3, 1, 2), two, three, one) == dbits(5.0));
    CHECK(dd(as::dfnms(3, 1, 2), two, three, dbits(10.0)) == dbits(4.0));
    CHECK(dd(as::dfnma(3, 1, 2), two, three, one) == dbits(-7.0));

    // fesd: DP element i <- SP element 2i
    m.r(1) = words(0x3FC00000, 0x12345678, 0xC0000000, 0x9ABCDEF0);
    m.exec(as::fesd(3, 1));
    CHECK(gd(m.r(3), 0) == dbits(1.5));
    CHECK(gd(m.r(3), 1) == dbits(-2.0));
    m.r(1) = words(0x00000005, 0, kInfPat, 0);
    m.exec(as::fesd(3, 1));
    CHECK(gd(m.r(3), 0) == 0u); // SP denormal is zero
    CHECK(gd(m.r(3), 1) == 0x7FF0000000000000ull);

    // frds: SP element 2i <- DP element i rounded to nearest, odd elements zero
    m.r(1) = dwords(0x3FF0000010400000ull /* 1 + 2^-24 + 2^-30 */, dbits(2.0));
    m.exec(as::frds(3, 1));
    CHECK(hex(m.r(3)) == "3f800001 00000000 40000000 00000000");
    m.r(1) = dwords(dbits(1e300), dbits(-1e300));
    m.exec(as::frds(3, 1));
    CHECK(hex(m.r(3)) == "7f800000 00000000 ff800000 00000000");
}

TEST_CASE("fscrwr / fscrrd keep the architected FPSCR bits")
{
    Machine m;
    m.r(1) = splat(0xFFFFFFFF);
    m.exec(as::fscrwr(1));
    m.exec(as::fscrrd(3));
    CHECK(hex(m.r(3)) == "00000f07 00003f07 00003f07 00000f07");
}
