// Interpreter unit tests, floating point: single-precision rounding, fused multiply-add exactness, conversions
// (saturation, NaN, rounding modes), compares and FPCC, estimates, the FPSCR model and FP record forms.

#include <catch_amalgamated.hpp>

#include <cmath>

#include "ppu_test_util.hpp"
#include "pspocket/ppu/fpu.h"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::ppu;
using namespace pspocket::ppu::test;
namespace as = pspocket::recomp::ppu::as;

TEST_CASE("single-precision forms round the double result to float (nearest-even)")
{
    Machine m;
    m.set_f(1, 1.0);
    m.set_f(2, std::ldexp(1.0, -30));
    m.exec(as::fadds(3, 1, 2));
    CHECK(m.f(3) == 1.0);
    m.exec(as::fadd(3, 1, 2));
    CHECK(m.f(3) == 1.0 + std::ldexp(1.0, -30));

    m.set_f(2, std::ldexp(1.0, -24)); // exactly half an ulp of float at 1.0: tie to even
    m.exec(as::fadds(3, 1, 2));
    CHECK(m.f(3) == 1.0);
    m.set_f(2, std::ldexp(1.0, -24) + std::ldexp(1.0, -40));
    m.exec(as::fadds(3, 1, 2));
    CHECK(m.f(3) == 1.0 + std::ldexp(1.0, -23));

    m.set_f(1, 1.0 + std::ldexp(1.0, -12));
    m.exec(as::fmuls(3, 1, 1)); // 1 + 2^-11 + 2^-24: tie, even mantissa wins
    CHECK(m.f(3) == 1.0 + std::ldexp(1.0, -11));

    m.set_f(1, 1.0);
    m.set_f(2, 3.0);
    m.exec(as::fdivs(3, 1, 2));
    CHECK(m.f(3) == static_cast<double>(1.0f / 3.0f));
    m.exec(as::fdiv(3, 1, 2));
    CHECK(m.f(3) == 1.0 / 3.0);
    m.exec(as::frsp(4, 3));
    CHECK(m.f(4) == static_cast<double>(1.0f / 3.0f));
    m.set_f(5, 1e300);
    m.exec(as::frsp(4, 5));
    CHECK(std::isinf(m.f(4)));
    m.set_f(5, 1e-50);
    m.exec(as::frsp(4, 5));
    CHECK(m.f(4) == 0.0);

    m.set_f(5, 2.0);
    m.exec(as::fsqrts(4, 5));
    CHECK(m.f(4) == static_cast<double>(std::sqrt(2.0f)));
    m.exec(as::fsqrt(4, 5));
    CHECK(m.f(4) == std::sqrt(2.0));
    m.set_f(5, -1.0);
    m.exec(as::fsqrt(4, 5));
    CHECK(std::isnan(m.f(4)));
}

TEST_CASE("fmadd family is fused (one rounding)")
{
    Machine m;
    const double eps = std::ldexp(1.0, -30);
    m.set_f(1, 1.0 + eps);     // A
    m.set_f(2, 1.0 - eps);     // C
    m.set_f(3, -1.0);          // B
    m.exec(as::fmadd(4, 1, 2, 3));
    CHECK(m.f(4) == -std::ldexp(1.0, -60)); // A*C = 1 - 2^-60 exactly; unfused it would be 0
    m.exec(as::fmul(5, 1, 2));
    m.exec(as::fadd(5, 5, 3));
    CHECK(m.f(5) == 0.0);
    m.set_f(3, 1.0);
    m.exec(as::fmsub(4, 1, 2, 3));
    CHECK(m.f(4) == -std::ldexp(1.0, -60));
    m.exec(as::encode(Op::FNMSUB, as::Fields{.d = 4, .a = 1, .b = 3, .c = 2}));
    CHECK(m.f(4) == std::ldexp(1.0, -60));
    m.set_f(3, -1.0);
    m.exec(as::fnmadd(4, 1, 2, 3));
    CHECK(m.f(4) == std::ldexp(1.0, -60));
    m.exec(as::fmadds(4, 1, 2, 3));
    CHECK(m.f(4) == -std::ldexp(1.0, -60)); // a power of two survives rounding to float
}

TEST_CASE("fctiw/fctiwz: rounding mode, saturation, NaN, sign-extended 32-bit result (RPCS3)")
{
    Machine m;
    auto conv = [&](u32 insn, double v, u32 rn) {
        m.ctx.fpscr = rn;
        m.set_f(1, v);
        m.exec(insn);
        return m.fbits(2);
    };
    const u32 fctiwz = as::fctiwz(2, 1), fctiw = as::fctiw(2, 1);
    CHECK(conv(fctiwz, 3.7, 0) == 3);
    CHECK(conv(fctiwz, -3.7, 0) == static_cast<u64>(-3));
    CHECK(conv(fctiwz, 2147483647.9, 0) == 0x7FFFFFFF);
    CHECK(conv(fctiwz, 2147483648.0, 0) == 0x7FFFFFFF);
    CHECK(conv(fctiwz, -2147483649.0, 0) == 0xFFFFFFFF80000000ull);
    CHECK(conv(fctiwz, HUGE_VAL, 0) == 0x7FFFFFFF);
    CHECK(conv(fctiwz, -HUGE_VAL, 0) == 0xFFFFFFFF80000000ull);
    CHECK(conv(fctiwz, std::nan(""), 0) == 0xFFFFFFFF80000000ull);
    CHECK(conv(fctiwz, 2.9, 3) == 2); // fctiwz ignores RN

    CHECK(conv(fctiw, 2.5, PS_FPSCR_RN_NEAR) == 2);
    CHECK(conv(fctiw, 3.5, PS_FPSCR_RN_NEAR) == 4);
    CHECK(conv(fctiw, -2.5, PS_FPSCR_RN_NEAR) == static_cast<u64>(-2));
    CHECK(conv(fctiw, 2147483647.5, PS_FPSCR_RN_NEAR) == 0x7FFFFFFF); // rounds to 2^31, saturates
    CHECK(conv(fctiw, 2.9, PS_FPSCR_RN_ZERO) == 2);
    CHECK(conv(fctiw, 2.1, PS_FPSCR_RN_PINF) == 3);
    CHECK(conv(fctiw, -2.9, PS_FPSCR_RN_PINF) == static_cast<u64>(-2));
    CHECK(conv(fctiw, -2.1, PS_FPSCR_RN_MINF) == static_cast<u64>(-3));
    CHECK(conv(fctiw, 2.9, PS_FPSCR_RN_MINF) == 2);

    const u32 fctid = as::fctid(2, 1), fctidz = as::fctidz(2, 1);
    CHECK(conv(fctid, 1e19, 0) == 0x7FFFFFFFFFFFFFFFull);
    CHECK(conv(fctid, -1e19, 0) == 0x8000000000000000ull);
    CHECK(conv(fctid, std::nan(""), 0) == 0x8000000000000000ull);
    CHECK(conv(fctid, -9223372036854775808.0, 0) == 0x8000000000000000ull);
    CHECK(conv(fctid, 2.5, 0) == 2);
    CHECK(conv(fctidz, -2.5, 0) == static_cast<u64>(-2));
    CHECK(conv(fctid, 2251799813685248.5, 0) == 2251799813685248ull); // 2^51 + 0.5: ties to even
    CHECK(conv(fctid, 2251799813685249.5, 0) == 2251799813685250ull);

    // fctiwz + stfiwx: the idiom compilers use for (int)x
    m.set_f(1, -7.9);
    m.exec(as::fctiwz(2, 1));
    m.ctx.gpr[3] = Machine::kData;
    m.ctx.gpr[4] = 0;
    m.exec(as::stfiwx(2, 4, 3));
    CHECK(m.r32(Machine::kData) == static_cast<u32>(-7));
}

TEST_CASE("fcfid converts the 64-bit integer bits, rounding to nearest")
{
    Machine m;
    m.set_fbits(1, static_cast<u64>(-5));
    m.exec(as::fcfid(2, 1));
    CHECK(m.f(2) == -5.0);
    m.set_fbits(1, (1ull << 53) + 1);
    m.exec(as::fcfid(2, 1));
    CHECK(m.f(2) == 9007199254740992.0);
}

TEST_CASE("fcmpu/fcmpo set the CR field and FPSCR[FPCC]")
{
    Machine m;
    auto cmp = [&](double a, double b) {
        m.set_f(1, a);
        m.set_f(2, b);
        m.exec(as::fcmpu(6, 1, 2));
        return m.crf(6);
    };
    CHECK(cmp(1.0, 2.0) == LT);
    CHECK((m.ctx.fpscr & PS_FPSCR_FPCC) == PS_FPSCR_FL);
    CHECK(cmp(3.0, 2.0) == GT);
    CHECK((m.ctx.fpscr & PS_FPSCR_FPCC) == PS_FPSCR_FG);
    CHECK(cmp(-0.0, 0.0) == EQ);
    CHECK((m.ctx.fpscr & PS_FPSCR_FPCC) == PS_FPSCR_FE);
    CHECK(cmp(std::nan(""), 1.0) == SO); // unordered
    CHECK((m.ctx.fpscr & PS_FPSCR_FPCC) == PS_FPSCR_FU);
    m.ctx.fpscr = 0xFFFF0FFFu & ~PS_FPSCR_RESERVED;
    m.set_f(1, 1.0);
    m.set_f(2, 1.0);
    m.exec(as::fcmpo(0, 1, 2));
    CHECK(m.crf(0) == EQ);
    CHECK(m.ctx.fpscr == ((0xFFFF0FFFu & ~PS_FPSCR_RESERVED) | PS_FPSCR_FE)); // only FPCC changes
}

TEST_CASE("fsel, fmr/fneg/fabs/fnabs are bitwise (NaN payloads kept)")
{
    Machine m;
    m.set_f(2, 10.0); // C
    m.set_f(3, 20.0); // B
    auto sel = [&](double a) {
        m.set_f(1, a);
        m.exec(as::fsel(4, 1, 2, 3));
        return m.f(4);
    };
    CHECK(sel(0.0) == 10.0);
    CHECK(sel(-0.0) == 10.0);
    CHECK(sel(-1.0) == 20.0);
    CHECK(sel(std::nan("")) == 20.0);

    m.set_fbits(1, 0x7FF8000000000123ull);
    m.exec(as::fneg(2, 1));
    CHECK(m.fbits(2) == 0xFFF8000000000123ull);
    m.exec(as::fabs_(3, 2));
    CHECK(m.fbits(3) == 0x7FF8000000000123ull);
    m.exec(as::encode(Op::FNABS, as::Fields{.d = 4, .b = 1}));
    CHECK(m.fbits(4) == 0xFFF8000000000123ull);
    m.set_fbits(1, 0x7FF0000000000001ull); // signalling NaN
    m.exec(as::fmr(5, 1));
    CHECK(m.fbits(5) == 0x7FF0000000000001ull);
}

TEST_CASE("fres / frsqrte estimates match RPCS3's tables")
{
    Machine m;
    auto est = [&](u32 insn, double v) {
        m.set_f(1, v);
        m.exec(insn);
        return m.fbits(2);
    };
    const u32 fres = as::fres(2, 1), frsqrte = as::frsqrte(2, 1);
    CHECK(est(fres, 2.0) == 0x3FDFE00000000000ull); // 0.498046875
    CHECK(est(fres, 1.0) == 0x3FEFE00000000000ull);
    CHECK(est(fres, 0.0) == 0x7FF0000000000000ull);
    CHECK(est(fres, -0.0) == 0xFFF0000000000000ull);
    CHECK(est(fres, HUGE_VAL) == 0);
    CHECK(est(fres, -HUGE_VAL) == 0x8000000000000000ull);
    CHECK(std::isnan(std::bit_cast<double>(est(fres, std::nan("")))));
    CHECK(std::abs(std::bit_cast<double>(est(fres, 3.0)) - 1.0 / 3.0) < 1.0 / 3.0 / 128);

    CHECK(est(frsqrte, 1.0) == 0x3FEF100000000000ull);
    CHECK(est(frsqrte, 4.0) == 0x3FDF100000000000ull);
    CHECK(est(frsqrte, 0.25) == 0x3FFF100000000000ull);
    CHECK(est(frsqrte, -1.0) == 0x7FF8000000000000ull);
    CHECK(est(frsqrte, 0.0) == 0x7FF0000000000000ull);
    CHECK(est(frsqrte, -0.0) == 0xFFF0000000000000ull);
    CHECK(est(frsqrte, HUGE_VAL) == 0);
    CHECK(std::abs(std::bit_cast<double>(est(frsqrte, 2.0)) - 1.0 / std::sqrt(2.0)) < 0.03);
}

TEST_CASE("FPSCR model: mtfsfi/mtfsb0/mtfsb1/mtfsf/mffs/mcrfs, summary bits, FP record forms")
{
    Machine m;
    m.ctx.fpscr = 0;
    m.exec(as::mtfsfi(7, 3));
    CHECK(m.ctx.fpscr == 3); // RN = round toward -inf
    m.exec(as::mffs(1));
    CHECK(m.fbits(1) == 3);

    m.exec(as::mtfsb1(3)); // OX: an exception bit 0 -> 1 also sets FX
    CHECK(m.ctx.fpscr == (PS_FPSCR_FX | PS_FPSCR_OX | 3));
    m.exec(as::mtfsb1(25)); // OE: enabled overflow -> FEX
    CHECK(m.ctx.fpscr == (PS_FPSCR_FX | PS_FPSCR_FEX | PS_FPSCR_OX | PS_FPSCR_OE | 3));
    m.exec(as::mtfsb1(7)); // VXSNAN -> VX summary
    CHECK(m.ctx.fpscr == 0xF1000043u);
    m.exec(as::mtfsb0(7));
    CHECK(m.ctx.fpscr == 0xD0000043u);
    m.exec(as::mtfsb1(1)); // FEX cannot be set explicitly
    m.exec(as::mtfsb0(2)); // nor VX cleared
    CHECK(m.ctx.fpscr == 0xD0000043u);

    // Rc=1: CR1 = FX FEX VX OX
    m.set_f(1, 1.0);
    m.set_f(2, 2.0);
    m.exec(as::fadd(3, 1, 2, true));
    CHECK(m.f(3) == 3.0);
    CHECK(m.crf(1) == 0xD);

    // mcrfs copies a field and clears the exception bits it copied
    m.exec(as::encode(Op::MCRFS, as::Fields{.crfd = 2, .crfs = 0}));
    CHECK(m.crf(2) == 0xD);
    CHECK(m.ctx.fpscr == 0x00000043u);

    m.set_fbits(4, 0x123456788200F0F3ull);
    m.exec(as::mtfsf(0xFF, 4)); // all fields from the low word
    CHECK(m.ctx.fpscr == 0x8200F0F3u);
    m.exec(as::mtfsfi(0, 9)); // FX=1, OX=1 (FEX/VX ignored), OE set -> FEX
    CHECK(m.ctx.fpscr == 0xD200F0F3u);
    m.exec(as::mtfsf(0x01, 4, true)); // only field 7
    CHECK(m.ctx.fpscr == 0xD200F0F3u);
    CHECK(m.crf(1) == 0xD);
}

TEST_CASE("FP loads and stores convert like the host (as RPCS3)")
{
    Machine m;
    const u32 d = Machine::kData;
    m.ctx.gpr[3] = d;
    m.w32(d, 0x3FC00000); // 1.5f
    m.exec(as::lfs(1, 0, 3));
    CHECK(m.f(1) == 1.5);
    m.w32(d, 0x7F800001); // signalling NaN float: quieted by the conversion
    m.exec(as::lfs(1, 0, 3));
    CHECK(m.fbits(1) == 0x7FF8000020000000ull);
    m.set_f(2, 1.0 / 3.0);
    m.exec(as::stfs(2, 4, 3)); // rounds (RPCS3); hardware would truncate
    CHECK(m.r32(d + 4) == 0x3EAAAAABu);
    m.set_fbits(2, 0x0123456789ABCDEFull);
    m.exec(as::stfd(2, 8, 3));
    CHECK(m.r64(d + 8) == 0x0123456789ABCDEFull);
    m.exec(as::lfd(3, 8, 3));
    CHECK(m.fbits(3) == 0x0123456789ABCDEFull);
    m.exec(as::lfdu(4, 8, 3));
    CHECK(m.ctx.gpr[3] == d + 8);
}
