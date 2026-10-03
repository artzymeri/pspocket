// Interpreter unit tests, integer instructions: hand-checked vectors for the cases that differ between "obvious"
// and Cell semantics (64-bit mode, carry/overflow, record forms, rotate masks, division edge cases per RPCS3).

#include <catch_amalgamated.hpp>

#include "ppu_test_util.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::ppu;
using namespace pspocket::ppu::test;
namespace as = pspocket::recomp::ppu::as;

TEST_CASE("add/addo/add.: overflow is 64-bit, CR0 compares the 64-bit result")
{
    Machine m;
    m.ctx.gpr[4] = 0x7FFFFFFFFFFFFFFFull;
    m.ctx.gpr[5] = 1;
    m.exec(as::add(3, 4, 5, true, true)); // addo.
    CHECK(m.ctx.gpr[3] == 0x8000000000000000ull);
    CHECK(m.ctx.xer_ov == 1);
    CHECK(m.ctx.xer_so == 1);
    CHECK(m.crf(0) == (LT | SO));

    // A 32-bit overflow is not an overflow in 64-bit mode, and the result is positive.
    m.ctx.xer_ov = m.ctx.xer_so = 0;
    m.ctx.gpr[4] = 0x7FFFFFFF;
    m.exec(as::add(3, 4, 5, true, true));
    CHECK(m.ctx.gpr[3] == 0x80000000ull);
    CHECK(m.ctx.xer_ov == 0);
    CHECK(m.crf(0) == GT);

    // OV is cleared by a non-overflowing o-form, SO stays sticky.
    m.ctx.xer_so = 1;
    m.ctx.xer_ov = 1;
    m.exec(as::add(3, 4, 5, true, false));
    CHECK(m.ctx.xer_ov == 0);
    CHECK(m.ctx.xer_so == 1);
}

TEST_CASE("addc/adde/addze carry is 64-bit")
{
    Machine m;
    m.ctx.gpr[4] = 0xFFFFFFFF;
    m.ctx.gpr[5] = 1;
    m.exec(as::addc(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 0x100000000ull);
    CHECK(m.ctx.xer_ca == 0);

    m.ctx.gpr[4] = ~0ull;
    m.exec(as::addc(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 0);
    CHECK(m.ctx.xer_ca == 1);

    m.ctx.gpr[6] = 10;
    m.ctx.gpr[7] = 20;
    m.exec(as::adde(8, 6, 7)); // 10 + 20 + CA(1)
    CHECK(m.ctx.gpr[8] == 31);
    CHECK(m.ctx.xer_ca == 0);

    // adde with carry-in producing carry-out only through the +1
    m.ctx.xer_ca = 1;
    m.ctx.gpr[6] = ~0ull;
    m.ctx.gpr[7] = 0;
    m.exec(as::adde(8, 6, 7));
    CHECK(m.ctx.gpr[8] == 0);
    CHECK(m.ctx.xer_ca == 1);

    m.ctx.xer_ca = 1;
    m.ctx.gpr[6] = 41;
    m.exec(as::addze(8, 6));
    CHECK(m.ctx.gpr[8] == 42);
    CHECK(m.ctx.xer_ca == 0);

    // addme: rA + ~0 + CA
    m.ctx.xer_ca = 0;
    m.ctx.gpr[6] = 0;
    m.exec(as::addme(8, 6));
    CHECK(m.ctx.gpr[8] == ~0ull);
    CHECK(m.ctx.xer_ca == 0);
    m.ctx.xer_ca = 1;
    m.exec(as::addme(8, 6));
    CHECK(m.ctx.gpr[8] == 0);
    CHECK(m.ctx.xer_ca == 1);
}

TEST_CASE("subf family: rB - rA, CA = no borrow")
{
    Machine m;
    m.ctx.gpr[4] = 5;
    m.ctx.gpr[5] = 3;
    m.exec(as::subfc(3, 4, 5)); // 3 - 5
    CHECK(m.ctx.gpr[3] == static_cast<u64>(-2));
    CHECK(m.ctx.xer_ca == 0);
    m.exec(as::subfc(3, 5, 4)); // 5 - 3
    CHECK(m.ctx.gpr[3] == 2);
    CHECK(m.ctx.xer_ca == 1);
    m.exec(as::subfc(3, 4, 4));
    CHECK(m.ctx.gpr[3] == 0);
    CHECK(m.ctx.xer_ca == 1);

    // subfe: ~rA + rB + CA (borrow chain); subfe r,r,r = CA - 1
    m.ctx.xer_ca = 0;
    m.exec(as::subfe(3, 4, 4));
    CHECK(m.ctx.gpr[3] == ~0ull);
    m.ctx.xer_ca = 1;
    m.exec(as::subfe(3, 4, 4));
    CHECK(m.ctx.gpr[3] == 0);

    // subfo: INT64_MIN - 1 overflows
    m.ctx.gpr[4] = 1;
    m.ctx.gpr[5] = 0x8000000000000000ull;
    m.exec(as::subf(3, 4, 5, true));
    CHECK(m.ctx.gpr[3] == 0x7FFFFFFFFFFFFFFFull);
    CHECK(m.ctx.xer_ov == 1);

    // subfic: imm - rA
    m.ctx.gpr[4] = 0;
    m.exec(as::subfic(3, 4, 0));
    CHECK(m.ctx.gpr[3] == 0);
    CHECK(m.ctx.xer_ca == 1);
    m.ctx.gpr[4] = 1;
    m.exec(as::subfic(3, 4, 0));
    CHECK(m.ctx.gpr[3] == ~0ull);
    CHECK(m.ctx.xer_ca == 0);
    m.ctx.gpr[4] = 10;
    m.exec(as::subfic(3, 4, -1)); // -1 - 10
    CHECK(m.ctx.gpr[3] == static_cast<u64>(-11));
    CHECK(m.ctx.xer_ca == 1);

    // subfze / subfme
    m.ctx.xer_ca = 1;
    m.ctx.gpr[4] = 0;
    m.exec(as::subfze(3, 4)); // ~0 + 0 + 1
    CHECK(m.ctx.gpr[3] == 0);
    CHECK(m.ctx.xer_ca == 1);
    m.ctx.xer_ca = 0;
    m.exec(as::subfme(3, 4)); // ~0 + ~0 + 0
    CHECK(m.ctx.gpr[3] == static_cast<u64>(-2));
    CHECK(m.ctx.xer_ca == 1);

    // neg INT64_MIN overflows and stays INT64_MIN
    m.ctx.xer_ov = 0;
    m.ctx.gpr[4] = 0x8000000000000000ull;
    m.exec(as::neg(3, 4, true, true));
    CHECK(m.ctx.gpr[3] == 0x8000000000000000ull);
    CHECK(m.ctx.xer_ov == 1);
    CHECK(m.crf(0) == (LT | SO));
}

TEST_CASE("addi/addis/addic/mulli immediates")
{
    Machine m;
    m.ctx.gpr[0] = 1234;
    m.exec(as::addi(3, 0, -5)); // li: rA = 0 means literal 0
    CHECK(m.ctx.gpr[3] == static_cast<u64>(-5));
    m.exec(as::addis(3, 0, -1)); // lis r3,-1
    CHECK(m.ctx.gpr[3] == 0xFFFFFFFFFFFF0000ull);
    m.ctx.gpr[4] = 0x100000000ull;
    m.exec(as::addis(3, 4, 0x7FFF));
    CHECK(m.ctx.gpr[3] == 0x17FFF0000ull);

    m.ctx.gpr[4] = ~0ull;
    m.exec(as::addic(3, 4, 1));
    CHECK(m.ctx.gpr[3] == 0);
    CHECK(m.ctx.xer_ca == 1);
    m.ctx.gpr[4] = 5;
    m.exec(as::addic_(3, 4, -10));
    CHECK(m.ctx.gpr[3] == static_cast<u64>(-5));
    CHECK(m.ctx.xer_ca == 0);
    CHECK(m.crf(0) == LT);

    m.ctx.gpr[4] = 0x4000000000000000ull;
    m.exec(as::mulli(3, 4, 4));
    CHECK(m.ctx.gpr[3] == 0);
    m.ctx.gpr[4] = 7;
    m.exec(as::mulli(3, 4, -3));
    CHECK(m.ctx.gpr[3] == static_cast<u64>(-21));
}

TEST_CASE("multiplies: 32/64-bit products, high halves, overflow")
{
    Machine m;
    m.ctx.gpr[4] = 0x10000;
    m.ctx.gpr[5] = 0x10000;
    m.exec(as::mullw(3, 4, 5, true));
    CHECK(m.ctx.gpr[3] == 0x100000000ull); // full 64-bit product of the low words
    CHECK(m.ctx.xer_ov == 1);              // does not fit in 32 bits

    m.ctx.gpr[4] = 0xDEADBEEFFFFFFFFFull; // low word -1
    m.ctx.gpr[5] = 2;
    m.exec(as::mullw(3, 4, 5, true));
    CHECK(m.ctx.gpr[3] == static_cast<u64>(-2));
    CHECK(m.ctx.xer_ov == 0);

    m.ctx.gpr[4] = 0x80000000;
    m.ctx.gpr[5] = 0x80000000;
    m.exec(as::mulhw(3, 4, 5)); // (-2^31)^2 = 2^62
    CHECK(m.ctx.gpr[3] == 0x40000000);
    m.ctx.gpr[4] = 0xFFFFFFFF;
    m.ctx.gpr[5] = 1;
    m.exec(as::mulhw(3, 4, 5)); // -1 * 1: high word -1, sign-extended (RPCS3)
    CHECK(m.ctx.gpr[3] == ~0ull);
    m.ctx.gpr[5] = 0xFFFFFFFF;
    m.exec(as::mulhwu(3, 4, 5)); // 0xFFFFFFFE00000001
    CHECK(m.ctx.gpr[3] == 0xFFFFFFFEull);

    m.ctx.gpr[4] = ~0ull;
    m.ctx.gpr[5] = ~0ull;
    m.exec(as::mulhdu(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 0xFFFFFFFFFFFFFFFEull);
    m.exec(as::mulhd(3, 4, 5)); // (-1)(-1) = 1
    CHECK(m.ctx.gpr[3] == 0);
    m.ctx.gpr[4] = 1ull << 32;
    m.ctx.gpr[5] = 1ull << 32;
    m.ctx.xer_ov = 0;
    m.exec(as::mulld(3, 4, 5, true));
    CHECK(m.ctx.gpr[3] == 0);
    CHECK(m.ctx.xer_ov == 1);
    m.ctx.gpr[4] = static_cast<u64>(-3);
    m.ctx.gpr[5] = 5;
    m.exec(as::mulld(3, 4, 5, true));
    CHECK(m.ctx.gpr[3] == static_cast<u64>(-15));
    CHECK(m.ctx.xer_ov == 0);
}

TEST_CASE("divides: truncation, zero-extended 32-bit quotients, undefined cases are 0 (RPCS3)")
{
    Machine m;
    m.ctx.gpr[4] = 7;
    m.ctx.gpr[5] = static_cast<u64>(-2);
    m.exec(as::divw(3, 4, 5, false, true));
    CHECK(m.ctx.gpr[3] == 0xFFFFFFFDull); // -3 as a zero-extended word
    CHECK(m.crf(0) == GT);                // positive as a 64-bit value

    m.ctx.gpr[4] = 0x80000000;
    m.ctx.gpr[5] = 0xFFFFFFFF;
    m.exec(as::divw(3, 4, 5, true));
    CHECK(m.ctx.gpr[3] == 0);
    CHECK(m.ctx.xer_ov == 1);

    m.ctx.xer_ov = 0;
    m.ctx.gpr[5] = 0;
    m.exec(as::divw(3, 4, 5, true));
    CHECK(m.ctx.gpr[3] == 0);
    CHECK(m.ctx.xer_ov == 1);

    m.ctx.xer_ov = 0;
    m.ctx.gpr[4] = 0xFFFFFFFF;
    m.ctx.gpr[5] = 2;
    m.exec(as::divwu(3, 4, 5, true));
    CHECK(m.ctx.gpr[3] == 0x7FFFFFFF);
    CHECK(m.ctx.xer_ov == 0);
    m.ctx.gpr[5] = 0x100000000ull; // low word 0
    m.exec(as::divwu(3, 4, 5, true));
    CHECK(m.ctx.gpr[3] == 0);
    CHECK(m.ctx.xer_ov == 1);

    m.ctx.xer_ov = 0;
    m.ctx.gpr[4] = 0x8000000000000000ull;
    m.ctx.gpr[5] = ~0ull;
    m.exec(as::divd(3, 4, 5, true));
    CHECK(m.ctx.gpr[3] == 0);
    CHECK(m.ctx.xer_ov == 1);
    m.ctx.gpr[4] = static_cast<u64>(-9);
    m.ctx.gpr[5] = 2;
    m.exec(as::divd(3, 4, 5));
    CHECK(m.ctx.gpr[3] == static_cast<u64>(-4));
    m.ctx.gpr[5] = 0;
    m.exec(as::divdu(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 0);
}

TEST_CASE("record forms copy XER[SO] into CR0; andi. always records")
{
    Machine m;
    m.ctx.xer_so = 1;
    m.ctx.gpr[4] = 0xF0;
    m.exec(as::andi_(3, 4, 0x0F));
    CHECK(m.ctx.gpr[3] == 0);
    CHECK(m.crf(0) == (EQ | SO));
    m.ctx.xer_so = 0;
    m.ctx.gpr[4] = 0xFFFFFFFF00000000ull;
    m.exec(as::or_(3, 4, 4, true)); // mr. : negative as 64-bit
    CHECK(m.crf(0) == LT);
    m.exec(as::andis_(3, 4, 0x8000)); // high word ignored: 0
    CHECK(m.ctx.gpr[3] == 0);
    CHECK(m.crf(0) == EQ);
}

TEST_CASE("compares: 32-bit vs 64-bit, signed vs logical, SO copied")
{
    Machine m;
    m.ctx.gpr[4] = 0x00000001FFFFFFFFull;
    m.ctx.gpr[5] = 0;
    m.exec(as::cmpw(1, 4, 5)); // low word -1 < 0
    CHECK(m.crf(1) == LT);
    m.exec(as::cmpd(1, 4, 5));
    CHECK(m.crf(1) == GT);
    m.exec(as::cmplw(2, 4, 5)); // 0xFFFFFFFF > 0
    CHECK(m.crf(2) == GT);
    m.ctx.xer_so = 1;
    m.exec(as::cmpwi(7, 4, -1));
    CHECK(m.crf(7) == (EQ | SO));
    m.ctx.xer_so = 0;
    m.exec(as::cmplwi(0, 4, 0xFFFF));
    CHECK(m.crf(0) == GT);
    m.ctx.gpr[4] = static_cast<u64>(-5);
    m.exec(as::cmpdi(3, 4, -4));
    CHECK(m.crf(3) == LT);
    m.exec(as::cmpldi(3, 4, 5));
    CHECK(m.crf(3) == GT);
}

TEST_CASE("32-bit rotates use ROTL32 of the duplicated word; wrapping masks reach the high word")
{
    Machine m;
    m.ctx.gpr[4] = 0xAAAAAAAA12345678ull;
    m.exec(as::rlwinm(3, 4, 8, 24, 31)); // extract the top byte of the low word
    CHECK(m.ctx.gpr[3] == 0x12);
    m.exec(as::rlwinm(3, 4, 0, 28, 3)); // mb > me: mask 0xFFFFFFFF_F000000F over dup32
    CHECK(m.ctx.gpr[3] == 0x1234567810000008ull);
    m.exec(as::slwi(3, 4, 4));
    CHECK(m.ctx.gpr[3] == 0x23456780);
    m.exec(as::srwi(3, 4, 4));
    CHECK(m.ctx.gpr[3] == 0x01234567);

    m.ctx.gpr[3] = 0xFFFFFFFFFFFFFFFFull;
    m.ctx.gpr[4] = 0xBEEF;
    m.exec(as::rlwimi(3, 4, 16, 0, 15)); // insert the low halfword into the high half of the low word
    CHECK(m.ctx.gpr[3] == 0xFFFFFFFFBEEFFFFFull);

    m.ctx.gpr[4] = 0x80000001;
    m.ctx.gpr[5] = 33; // rotate count is rB & 31
    m.exec(as::rlwnm(3, 4, 5, 0, 31));
    CHECK(m.ctx.gpr[3] == 0x00000003);
}

TEST_CASE("64-bit rotates")
{
    Machine m;
    m.ctx.gpr[4] = 0xDEADBEEF12345678ull;
    m.exec(as::clrldi(3, 4, 32));
    CHECK(m.ctx.gpr[3] == 0x12345678);
    m.exec(as::sldi(3, 4, 4));
    CHECK(m.ctx.gpr[3] == 0xEADBEEF123456780ull);
    m.exec(as::srdi(3, 4, 60));
    CHECK(m.ctx.gpr[3] == 0xD);
    m.exec(as::rldic(3, 4, 8, 16)); // rotl 8, mask 16..55
    CHECK(m.ctx.gpr[3] == 0x0000EF1234567800ull);
    m.ctx.gpr[3] = 0x1111111122222222ull;
    m.exec(as::rldimi(3, 4, 32, 0)); // low word of r4 into the high word of r3
    CHECK(m.ctx.gpr[3] == 0x1234567822222222ull);
    m.ctx.gpr[5] = 68; // rB & 63 = 4
    m.exec(as::rldcl(3, 4, 5, 0));
    CHECK(m.ctx.gpr[3] == 0xEADBEEF12345678Dull);
    m.exec(as::rldcr(3, 4, 5, 3)); // rotl 4, keep IBM bits 0..3
    CHECK(m.ctx.gpr[3] == 0xE000000000000000ull);
}

TEST_CASE("shifts: 32-bit shifts zero-extend, shift counts 32..63 / 64..127, algebraic carry")
{
    Machine m;
    m.ctx.gpr[4] = 0xFFFFFFFF80000001ull;
    m.ctx.gpr[5] = 31;
    m.exec(as::slw(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 0x80000000);
    m.ctx.gpr[5] = 32;
    m.exec(as::slw(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 0);
    m.ctx.gpr[5] = 64 + 1; // only rB & 63 counts for slw/srw
    m.exec(as::slw(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 2);
    m.ctx.gpr[5] = 31;
    m.exec(as::srw(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 1);

    m.ctx.gpr[4] = 0x80000000; // negative word
    m.ctx.gpr[5] = 4;
    m.exec(as::sraw(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 0xFFFFFFFFF8000000ull);
    CHECK(m.ctx.xer_ca == 0);
    m.ctx.gpr[4] = 0x80000001;
    m.ctx.gpr[5] = 1;
    m.exec(as::sraw(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 0xFFFFFFFFC0000000ull);
    CHECK(m.ctx.xer_ca == 1);
    m.ctx.gpr[5] = 40;
    m.exec(as::sraw(3, 4, 5));
    CHECK(m.ctx.gpr[3] == ~0ull);
    CHECK(m.ctx.xer_ca == 1);
    m.ctx.gpr[4] = 0x7FFFFFFF;
    m.exec(as::sraw(3, 4, 5, true));
    CHECK(m.ctx.gpr[3] == 0);
    CHECK(m.ctx.xer_ca == 0);
    CHECK(m.crf(0) == EQ);

    m.ctx.gpr[4] = 0xFFFFFFFF7FFFFFFFull;
    m.exec(as::srawi(3, 4, 0)); // extsw, CA 0
    CHECK(m.ctx.gpr[3] == 0x7FFFFFFF);
    CHECK(m.ctx.xer_ca == 0);

    m.ctx.gpr[4] = 0x8000000000000001ull;
    m.exec(as::sradi(3, 4, 63));
    CHECK(m.ctx.gpr[3] == ~0ull);
    CHECK(m.ctx.xer_ca == 1);
    m.ctx.gpr[5] = 64;
    m.exec(as::srad(3, 4, 5));
    CHECK(m.ctx.gpr[3] == ~0ull);
    CHECK(m.ctx.xer_ca == 1);
    m.ctx.gpr[5] = 64;
    m.exec(as::sld(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 0);
    m.ctx.gpr[5] = 127;
    m.exec(as::srd(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 0);
    m.ctx.gpr[5] = 63;
    m.exec(as::srd(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 1);
}

TEST_CASE("count leading zeros and sign extension")
{
    Machine m;
    m.ctx.gpr[4] = 0;
    m.exec(as::cntlzw(3, 4));
    CHECK(m.ctx.gpr[3] == 32);
    m.exec(as::cntlzd(3, 4));
    CHECK(m.ctx.gpr[3] == 64);
    m.ctx.gpr[4] = 0xFFFFFFFF00000001ull;
    m.exec(as::cntlzw(3, 4)); // only the low word counts
    CHECK(m.ctx.gpr[3] == 31);
    m.exec(as::cntlzd(3, 4));
    CHECK(m.ctx.gpr[3] == 0);
    m.ctx.gpr[4] = 0x1234567890ABCD80ull;
    m.exec(as::extsb(3, 4));
    CHECK(m.ctx.gpr[3] == 0xFFFFFFFFFFFFFF80ull);
    m.exec(as::extsh(3, 4));
    CHECK(m.ctx.gpr[3] == 0xFFFFFFFFFFFFCD80ull);
    m.exec(as::extsw(3, 4));
    CHECK(m.ctx.gpr[3] == 0xFFFFFFFF90ABCD80ull);
}

TEST_CASE("CR logical ops, mcrf, mfcr/mtcrf/mfocrf/mtocrf")
{
    Machine m;
    m.set_crf(0, 0b1010);
    m.exec(as::crlog(Op::CRAND, 31, 0, 2)); // 1 & 1
    CHECK(m.ctx.cr[31] == 1);
    m.exec(as::crlog(Op::CRNOR, 30, 1, 3)); // !(0 | 0)
    CHECK(m.ctx.cr[30] == 1);
    m.exec(as::crlog(Op::CRXOR, 29, 0, 2)); // 1 ^ 1
    CHECK(m.ctx.cr[29] == 0);
    m.exec(as::crlog(Op::CREQV, 28, 0, 1)); // !(1 ^ 0)
    CHECK(m.ctx.cr[28] == 0);
    m.exec(as::crlog(Op::CRANDC, 27, 0, 1)); // 1 & !0
    CHECK(m.ctx.cr[27] == 1);
    m.exec(as::crlog(Op::CRORC, 26, 1, 0)); // 0 | !1
    CHECK(m.ctx.cr[26] == 0);
    m.exec(as::crlog(Op::CRNAND, 25, 0, 2)); // !(1 & 1)
    CHECK(m.ctx.cr[25] == 0);
    m.exec(as::crlog(Op::CROR, 24, 1, 2)); // 0 | 1
    CHECK(m.ctx.cr[24] == 1);

    m.exec(as::mcrf(5, 0));
    CHECK(m.crf(5) == 0b1010);

    for (u32 f = 0; f < 8; f++) m.set_crf(f, f + 1);
    m.exec(as::mfcr(3));
    CHECK(m.ctx.gpr[3] == 0x12345678);
    m.exec(as::mfocrf(4, 0x02)); // field 6 only
    CHECK(m.ctx.gpr[4] == 0x70);

    m.ctx.gpr[5] = 0xFFFFFFFF9ABCDEF1ull;
    m.exec(as::mtcrf(0x81, 5)); // fields 0 and 7
    CHECK(m.crf(0) == 0x9);
    CHECK(m.crf(7) == 0x1);
    CHECK(m.crf(1) == 2); // untouched
    m.exec(as::mtocrf(0x20, 5)); // field 2
    CHECK(m.crf(2) == 0xB);
}

TEST_CASE("SPR moves: XER layout, LR, CTR, VRSAVE")
{
    Machine m;
    m.ctx.gpr[4] = 0xFFFFFFFFE00000FFull;
    m.exec(as::mtxer(4));
    CHECK(m.ctx.xer_so == 1);
    CHECK(m.ctx.xer_ov == 1);
    CHECK(m.ctx.xer_ca == 1);
    CHECK(m.ctx.xer_bc == 0x7F);
    m.exec(as::mfxer(3));
    CHECK(m.ctx.gpr[3] == 0xE000007F);

    m.ctx.gpr[4] = 0x123456789ABCDEF0ull;
    m.exec(as::mtlr(4));
    CHECK(m.ctx.lr == 0x123456789ABCDEF0ull); // LR holds 64 bits
    m.exec(as::mtctr(4));
    m.exec(as::mfctr(5));
    CHECK(m.ctx.gpr[5] == 0x123456789ABCDEF0ull);
    m.exec(as::mtspr(kSprVrsave, 4));
    CHECK(m.ctx.vrsave == 0x9ABCDEF0u);
    m.exec(as::mfspr(6, kSprVrsave));
    CHECK(m.ctx.gpr[6] == 0x9ABCDEF0u);
}
