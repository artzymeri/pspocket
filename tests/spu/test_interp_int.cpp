// Hand-checked interpreter vectors: integer, logical, shuffle, shift/rotate, mask and gather instructions.
// Expected values are worked out from the SPU ISA definitions (guest element order), not from the implementation.

#include <catch_amalgamated.hpp>

#include "pspocket/recomp/spu/asm.hpp"
#include "spu_test_util.hpp"

using namespace pspocket;
using namespace pspocket::spu;
using namespace pspocket::spu::test;
namespace as = pspocket::recomp::spu::as;

TEST_CASE("constant formation")
{
    Machine m;
    m.exec(as::il(3, -2));
    CHECK(hex(m.r(3)) == "fffffffe fffffffe fffffffe fffffffe");
    m.exec(as::ilhu(3, 0x1234));
    CHECK(hex(m.r(3)) == "12340000 12340000 12340000 12340000");
    m.exec(as::iohl(3, 0xABCD));
    CHECK(hex(m.r(3)) == "1234abcd 1234abcd 1234abcd 1234abcd");
    m.exec(as::ilh(4, 0x8001));
    CHECK(hex(m.r(4)) == "80018001 80018001 80018001 80018001");
    m.exec(as::ila(5, 0x3FFFF));
    CHECK(hex(m.r(5)) == "0003ffff 0003ffff 0003ffff 0003ffff");
    m.exec(as::fsmbi(6, 0xF00F)); // guest byte i = 0xFF when bit 15 - i is set
    CHECK(hex(m.r(6)) == "ffffffff 00000000 00000000 ffffffff");
    m.exec(as::fsmbi(6, 0x8001));
    CHECK(hex(m.r(6)) == "ff000000 00000000 00000000 000000ff");
}

TEST_CASE("carries and borrows")
{
    Machine m;
    m.r(1) = words(0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0);
    m.r(2) = words(1, 0x80000000, 1, 0);
    m.exec(as::cg(3, 1, 2));
    CHECK(hex(m.r(3)) == "00000001 00000001 00000000 00000000");

    // bg rt, ra, rb: 1 when rb - ra does not borrow (rb >= ra unsigned)
    m.r(1) = words(5, 3, 5, 0);
    m.r(2) = words(3, 5, 5, 0xFFFFFFFF);
    m.exec(as::bg(3, 1, 2));
    CHECK(hex(m.r(3)) == "00000000 00000001 00000001 00000001");

    // addx: ra + rb + (rt & 1)
    m.r(1) = words(0xFFFFFFFF, 1, 2, 3);
    m.r(2) = words(0, 1, 2, 3);
    m.r(3) = words(1, 0, 3, 2);
    m.exec(as::addx(3, 1, 2));
    CHECK(hex(m.r(3)) == "00000000 00000002 00000005 00000006");

    // cgx: carry of ra + rb + (rt & 1)
    m.r(1) = words(0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFF, 0x80000000);
    m.r(2) = words(0, 0, 0, 0x80000000);
    m.r(3) = words(1, 1, 0, 0);
    m.exec(as::cgx(3, 1, 2));
    CHECK(hex(m.r(3)) == "00000001 00000000 00000000 00000001");

    // sfx: rb - ra - (~rt & 1) (rt bit 0 is "no borrow in")
    m.r(1) = words(1, 1, 0, 5);
    m.r(2) = words(1, 1, 0, 3);
    m.r(3) = words(0, 1, 0, 1);
    m.exec(as::sfx(3, 1, 2));
    CHECK(hex(m.r(3)) == "ffffffff 00000000 ffffffff fffffffe");

    // bgx: 1 when rb - ra - (1 - (rt & 1)) >= 0
    m.r(1) = words(1, 1, 0, 0);
    m.r(2) = words(1, 1, 0, 0xFFFFFFFF);
    m.r(3) = words(0, 1, 0, 0);
    m.exec(as::bgx(3, 1, 2));
    CHECK(hex(m.r(3)) == "00000000 00000001 00000000 00000001");

    m.r(1) = words(1, 2, 0x80000000, 0);
    m.r(2) = words(3, 1, 0, 0);
    m.exec(as::sf(4, 1, 2)); // rb - ra
    CHECK(hex(m.r(4)) == "00000002 ffffffff 80000000 00000000");
    m.exec(as::sfi(4, 1, -1)); // I10 - ra
    CHECK(hex(m.r(4)) == "fffffffe fffffffd 7fffffff ffffffff");
    m.r(1) = words(0x0001FFFF, 0, 0, 0);
    m.exec(as::ahi(4, 1, -1));
    CHECK(gw(m.r(4), 0) == 0x0000FFFEu);
}

TEST_CASE("multiplies: signedness")
{
    Machine m;
    m.r(1) = words(0x1234FFFF, 0x0003FFFF, 0x7FFF8000, 0xFFFF0002);
    m.r(2) = words(0x00000002, 0x00050002, 0x80008000, 0xFFFF0003);
    m.exec(as::mpy(3, 1, 2)); // signed low halves
    CHECK(hex(m.r(3)) == "fffffffe fffffffe 40000000 00000006");
    m.exec(as::mpyu(3, 1, 2)); // unsigned low halves
    CHECK(hex(m.r(3)) == "0001fffe 0001fffe 40000000 00000006");
    m.exec(as::mpyh(3, 1, 2)); // (ra.hi * rb.lo) << 16
    CHECK(hex(m.r(3)) == "24680000 00060000 80000000 fffd0000");
    m.exec(as::mpyhh(3, 1, 2)); // signed high halves
    CHECK(hex(m.r(3)) == "00000000 0000000f c0008000 00000001");
    m.exec(as::mpyhhu(3, 1, 2)); // unsigned high halves
    CHECK(hex(m.r(3)) == "00000000 0000000f 3fff8000 fffe0001");
    m.exec(as::mpys(3, 1, 2)); // (signed lo * lo) >> 16, sign-extended
    CHECK(hex(m.r(3)) == "ffffffff ffffffff 00004000 00000000");

    m.r(5) = splat(100);
    m.exec(as::mpya(4, 1, 2, 5)); // signed lo*lo + rc
    CHECK(hex(m.r(4)) == "00000062 00000062 40000064 0000006a");
    m.r(4) = splat(1);
    m.exec(as::mpyhha(4, 1, 2)); // rt += signed hi*hi
    CHECK(hex(m.r(4)) == "00000001 00000010 c0008001 00000002");
    m.r(4) = splat(1);
    m.exec(as::mpyhhau(4, 1, 2));
    CHECK(hex(m.r(4)) == "00000001 00000010 3fff8001 fffe0002");

    m.exec(as::mpyi(3, 1, -3)); // signed lo * I10
    CHECK(hex(m.r(3)) == "00000003 00000003 00018000 fffffffa");
    m.exec(as::mpyui(3, 1, -1)); // unsigned lo * (I10 & 0xFFFF)
    CHECK(hex(m.r(3)) == "fffe0001 fffe0001 7fff8000 0001fffe");
}

TEST_CASE("logical, select, extend, count")
{
    Machine m;
    m.r(1) = words(0xF0F0F0F0, 0x12345678, 0, 0xFFFFFFFF);
    m.r(2) = words(0xFF00FF00, 0x0000FFFF, 0, 0x0F0F0F0F);
    m.exec(as::andc(3, 1, 2));
    CHECK(hex(m.r(3)) == "00f000f0 12340000 00000000 f0f0f0f0");
    m.exec(as::orc(3, 1, 2));
    CHECK(hex(m.r(3)) == "f0fff0ff ffff5678 ffffffff ffffffff");
    m.exec(as::eqv(3, 1, 2));
    CHECK(hex(m.r(3)) == "f00ff00f edcb5678 ffffffff 0f0f0f0f");
    m.exec(as::andbi(3, 1, 0x0F));
    CHECK(hex(m.r(3)) == "00000000 02040608 00000000 0f0f0f0f");
    m.exec(as::xorhi(3, 1, -1)); // halfword immediate sign-extended to 0xFFFF
    CHECK(hex(m.r(3)) == "0f0f0f0f edcba987 ffffffff 00000000");
    m.exec(as::ori(3, 1, -512)); // 0xFFFFFE00
    CHECK(hex(m.r(3)) == "fffffef0 fffffe78 fffffe00 ffffffff");
    m.exec(as::orx(3, 1));
    CHECK(hex(m.r(3)) == "ffffffff 00000000 00000000 00000000");

    m.r(4) = words(0xFFFF0000, 0x0000FFFF, 0xAAAAAAAA, 0);
    m.exec(as::selb(5, 1, 2, 4)); // (rc & rb) | (~rc & ra)
    CHECK(hex(m.r(5)) == "ff00f0f0 1234ffff 00000000 ffffffff");

    m.r(1) = words(0x00000080, 0x0000807F, 0x80000000, 0x00007FFF);
    m.exec(as::xsbh(3, 1));
    CHECK(hex(m.r(3)) == "0000ff80 0000007f 00000000 0000ffff");
    m.exec(as::xshw(3, 1));
    CHECK(hex(m.r(3)) == "00000080 ffff807f 00000000 00007fff");
    m.r(1) = words(0x12345678, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFF);
    m.exec(as::xswd(3, 1));
    CHECK(hex(m.r(3)) == "ffffffff 80000000 00000000 7fffffff");

    m.r(1) = words(0, 1, 0x80000000, 0x00010000);
    m.exec(as::clz(3, 1));
    CHECK(hex(m.r(3)) == "00000020 0000001f 00000000 0000000f");
    m.r(1) = words(0xFF000102, 0x7F800000, 0, 0xFFFFFFFF);
    m.exec(as::cntb(3, 1));
    CHECK(hex(m.r(3)) == "08000101 07010000 00000000 08080808");
}

TEST_CASE("byte arithmetic: avgb, absdb, sumb")
{
    Machine m;
    m.r(1) = bytes({1, 255, 0, 10, 1, 2, 3, 4, 0, 0, 0, 0, 255, 255, 255, 255});
    m.r(2) = bytes({2, 255, 1, 3, 5, 6, 7, 8, 0, 0, 0, 0, 1, 1, 1, 1});
    m.exec(as::avgb(3, 1, 2));
    CHECK(hex(m.r(3)) == "02ff0107 03040506 00000000 80808080");
    m.exec(as::absdb(3, 1, 2));
    CHECK(hex(m.r(3)) == "01000107 04040404 00000000 fefefefe");
    // sumb: upper halfword of each word = sum of rb's 4 bytes, lower = sum of ra's 4 bytes
    m.exec(as::sumb(3, 1, 2));
    CHECK(hex(m.r(3)) == "0105010a 001a000a 00000000 000403fc");
}

TEST_CASE("compares")
{
    Machine m;
    m.r(1) = words(0xFFFFFFFF, 5, 0x80000000, 7);
    m.r(2) = words(1, 5, 0x7FFFFFFF, 8);
    m.exec(as::cgt(3, 1, 2));
    CHECK(hex(m.r(3)) == "00000000 00000000 00000000 00000000");
    m.exec(as::clgt(3, 1, 2));
    CHECK(hex(m.r(3)) == "ffffffff 00000000 ffffffff 00000000");
    m.exec(as::ceq(3, 1, 2));
    CHECK(hex(m.r(3)) == "00000000 ffffffff 00000000 00000000");
    m.exec(as::cgti(3, 1, -2));
    CHECK(hex(m.r(3)) == "ffffffff ffffffff 00000000 ffffffff");
    m.exec(as::clgti(3, 1, -2)); // immediate 0xFFFFFFFE unsigned
    CHECK(hex(m.r(3)) == "ffffffff 00000000 00000000 00000000");
    m.r(1) = bytes({0x80, 0x7F, 0x00, 0xFF, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
    m.exec(as::cgtbi(3, 1, 0x01)); // signed bytes > 1
    CHECK(hex(m.r(3)) == "00ff0000 00ffffff ffffffff ffffffff");
    m.exec(as::clgtbi(3, 1, 0x7F)); // unsigned bytes > 0x7F
    CHECK(hex(m.r(3)) == "ff0000ff 00000000 00000000 00000000");
    m.exec(as::ceqbi(3, 1, 0xFF));
    CHECK(hex(m.r(3)) == "000000ff 00000000 00000000 00000000");
    m.r(1) = words(0xFFFF0001, 0, 0, 0);
    m.exec(as::cgthi(3, 1, -2)); // halfwords -1 > -2, 1 > -2
    CHECK(gw(m.r(3), 0) == 0xFFFFFFFFu);
    m.exec(as::clgthi(3, 1, -2)); // 0xFFFF > 0xFFFE, 1 > 0xFFFE
    CHECK(gw(m.r(3), 0) == 0xFFFF0000u);
}

TEST_CASE("shufb: selection and the special control values")
{
    Machine m;
    m.r(1) = bytes({0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF});
    m.r(2) = bytes({0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF});
    // 0x00-0x0F: ra byte, 0x10-0x1F: rb byte; bits 5-6 are ignored when bit 7 is clear (0x20 -> ra[0], 0x7F ->
    // rb[15]); 10xxxxxx -> 0x00, 110xxxxx -> 0xFF, 111xxxxx -> 0x80.
    m.r(3) = bytes({0x00, 0x0F, 0x10, 0x1F, 0x20, 0x7F, 0x80, 0x9F, 0xBF, 0xC0, 0xDF, 0xE0, 0xFF, 0x05, 0x15, 0x3A});
    m.exec(as::shufb(4, 1, 2, 3));
    CHECK(hex(m.r(4)) == "a0afb0bf a0bf0000 00ffff80 80a5b5ba");

    // Byte-swap each word (a common shufb idiom): control 03 02 01 00 07 06 05 04 ...
    m.r(3) = bytes({0x03, 0x02, 0x01, 0x00, 0x07, 0x06, 0x05, 0x04, 0x0B, 0x0A, 0x09, 0x08, 0x0F, 0x0E, 0x0D, 0x0C});
    m.exec(as::shufb(4, 1, 2, 3));
    CHECK(hex(m.r(4)) == "a3a2a1a0 a7a6a5a4 abaaa9a8 afaeadac");
}

TEST_CASE("insertion masks: cbd chd cwd cdd and the x forms")
{
    Machine m;
    m.r(1) = splat(0x10);
    m.exec(as::cbd(3, 1, 3)); // address 0x13: byte slot 3 takes ra's preferred byte (0x03)
    CHECK(hex(m.r(3)) == "10111203 14151617 18191a1b 1c1d1e1f");
    m.exec(as::chd(3, 1, 6)); // address 0x16: halfword slot at byte 6
    CHECK(hex(m.r(3)) == "10111213 14150203 18191a1b 1c1d1e1f");
    m.exec(as::cwd(3, 1, 8)); // address 0x18: word slot at byte 8
    CHECK(hex(m.r(3)) == "10111213 14151617 00010203 1c1d1e1f");
    m.exec(as::cdd(3, 1, -8)); // address 0x08: doubleword slot at byte 8
    CHECK(hex(m.r(3)) == "10111213 14151617 00010203 04050607");
    m.r(2) = splat(0x0D);
    m.exec(as::cbx(3, 1, 2)); // 0x10 + 0x0D
    CHECK(hex(m.r(3)) == "10111213 14151617 18191a1b 1c031e1f");
    m.exec(as::cwx(3, 1, 2)); // word slot at byte 12
    CHECK(hex(m.r(3)) == "10111213 14151617 18191a1b 00010203");
}

TEST_CASE("element shifts and rotates, including rotm/rotma masks")
{
    Machine m;
    m.r(1) = splat(0x80000001);
    m.r(2) = words(1, 31, 32, 64); // shl uses count & 0x3F: 64 -> 0
    m.exec(as::shl(3, 1, 2));
    CHECK(hex(m.r(3)) == "00000002 80000000 00000000 80000001");
    m.exec(as::rot(3, 1, 2)); // count mod 32
    CHECK(hex(m.r(3)) == "00000003 c0000000 80000001 80000001");

    // rotm: logical right shift by (0 - count) & 0x3F; >= 32 gives 0
    m.r(2) = words(static_cast<u32>(-4), 0, static_cast<u32>(-32), 60);
    m.exec(as::rotm(3, 1, 2));
    CHECK(hex(m.r(3)) == "08000000 80000001 00000000 08000000");
    // rotma: arithmetic, >= 32 fills with the sign
    m.r(2) = words(static_cast<u32>(-4), static_cast<u32>(-31), static_cast<u32>(-40), 1);
    m.exec(as::rotma(3, 1, 2));
    CHECK(hex(m.r(3)) == "f8000000 ffffffff ffffffff ffffffff");
    m.exec(as::rotmi(3, 1, -4));
    CHECK(gw(m.r(3), 0) == 0x08000000u);
    m.exec(as::rotmai(3, 1, -4));
    CHECK(gw(m.r(3), 0) == 0xF8000000u);
    m.exec(as::rotmi(3, 1, 0)); // count 0: unchanged
    CHECK(gw(m.r(3), 0) == 0x80000001u);
    m.exec(as::shli(3, 1, 4));
    CHECK(gw(m.r(3), 0) == 0x00000010u);
    m.exec(as::roti(3, 1, -1)); // rotate left by 31 == right by 1
    CHECK(gw(m.r(3), 0) == 0xC0000000u);

    // halfwords: counts masked to 5 bits, >= 16 gives 0 / sign
    m.r(1) = words(0x80018001, 0x80018001, 0x80018001, 0x80018001);
    m.r(2) = words(0x00010010, 0x000F001F, static_cast<u32>(0xFFFF0000), 0x00200000);
    m.exec(as::shlh(3, 1, 2));
    CHECK(hex(m.r(3)) == "00020000 80000000 00008001 80018001");
    m.exec(as::roth(3, 1, 2));
    CHECK(hex(m.r(3)) == "00038001 c000c000 c0008001 80018001");
    m.r(2) = words(0xFFFF0000u | 0xFFF0u, 0, 0, 0); // rothm counts: -1 -> 1, -16 -> 16
    m.exec(as::rothm(3, 1, 2));
    CHECK(gw(m.r(3), 0) == 0x40000000u);
    m.exec(as::rotmah(3, 1, 2));
    CHECK(gw(m.r(3), 0) == 0xC000FFFFu);
    m.exec(as::rothmi(3, 1, -15));
    CHECK(gw(m.r(3), 0) == 0x00010001u);
    m.exec(as::rotmahi(3, 1, -15));
    CHECK(gw(m.r(3), 0) == 0xFFFFFFFFu);
}

TEST_CASE("quadword shifts and rotates")
{
    Machine m;
    m.r(1) = bytes({0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF});
    m.r(2) = words(1, 0, 0, 0);
    m.exec(as::shlqby(3, 1, 2));
    CHECK(hex(m.r(3)) == "11223344 55667788 99aabbcc ddeeff00");
    m.exec(as::rotqby(3, 1, 2));
    CHECK(hex(m.r(3)) == "11223344 55667788 99aabbcc ddeeff00"); // byte 0 (0x00) wraps to the end
    m.r(2) = words(static_cast<u32>(-3), 0, 0, 0); // rotqmby: shift right by (0 - n) & 0x1F = 3 bytes
    m.exec(as::rotqmby(3, 1, 2));
    CHECK(hex(m.r(3)) == "00000000 11223344 55667788 99aabbcc");
    m.r(2) = words(16, 0, 0, 0);
    m.exec(as::shlqby(3, 1, 2)); // >= 16 bytes: zero
    CHECK(hex(m.r(3)) == "00000000 00000000 00000000 00000000");
    m.exec(as::rotqby(3, 1, 2)); // rotate count mod 16
    CHECK(hex(m.r(3)) == "00112233 44556677 8899aabb ccddeeff");
    m.r(2) = words(4 * 8, 0, 0, 0); // *bi forms: count in bits 24-28 (bytes = count >> 3)
    m.exec(as::shlqbybi(3, 1, 2));
    CHECK(hex(m.r(3)) == "44556677 8899aabb ccddeeff 00000000");
    m.r(2) = words(static_cast<u32>(-4 * 8), 0, 0, 0);
    m.exec(as::rotqmbybi(3, 1, 2));
    CHECK(hex(m.r(3)) == "00000000 00112233 44556677 8899aabb");
    m.exec(as::rotqbyi(3, 1, 15));
    CHECK(hex(m.r(3)) == "ff001122 33445566 778899aa bbccddee");
    m.exec(as::shlqbyi(3, 1, 0));
    CHECK(hex(m.r(3)) == "00112233 44556677 8899aabb ccddeeff");
    m.exec(as::rotqmbyi(3, 1, -1));
    CHECK(hex(m.r(3)) == "00001122 33445566 778899aa bbccddee");

    // bit forms: shift/rotate the whole 128-bit value by 0-7 bits
    m.r(1) = words(0x80000001, 0, 0, 0x00000001);
    m.r(2) = words(1, 0, 0, 0);
    m.exec(as::shlqbi(3, 1, 2));
    CHECK(hex(m.r(3)) == "00000002 00000000 00000000 00000002");
    m.exec(as::rotqbi(3, 1, 2)); // the MSB rotates into the LSB
    CHECK(hex(m.r(3)) == "00000002 00000000 00000000 00000003");
    m.r(2) = words(static_cast<u32>(-1), 0, 0, 0); // rotqmbi: right by (0 - n) & 7 = 1
    m.exec(as::rotqmbi(3, 1, 2));
    CHECK(hex(m.r(3)) == "40000000 80000000 00000000 00000000");
    m.exec(as::shlqbii(3, 1, 7));
    CHECK(hex(m.r(3)) == "00000080 00000000 00000000 00000080");
    m.exec(as::rotqbii(3, 1, 7));
    CHECK(hex(m.r(3)) == "00000080 00000000 00000000 000000c0");
    m.exec(as::rotqmbii(3, 1, -7));
    CHECK(hex(m.r(3)) == "01000000 02000000 00000000 00000000");
}

TEST_CASE("form select masks and gather bits")
{
    Machine m;
    m.r(1) = words(0x0000A5C3, 0, 0, 0);
    m.exec(as::fsm(3, 1)); // low 4 bits 0b0011: elements 2, 3
    CHECK(hex(m.r(3)) == "00000000 00000000 ffffffff ffffffff");
    m.exec(as::fsmh(3, 1)); // low 8 bits 0xC3 = 11000011
    CHECK(hex(m.r(3)) == "ffffffff 00000000 00000000 ffffffff");
    m.exec(as::fsmb(3, 1)); // low 16 bits 0xA5C3
    CHECK(hex(m.r(3)) == "ff00ff00 00ff00ff ffff0000 0000ffff");

    m.r(1) = words(1, 0, 3, 2); // LSBs 1 0 1 0
    m.exec(as::gb(3, 1));
    CHECK(hex(m.r(3)) == "0000000a 00000000 00000000 00000000");
    m.r(1) = words(0x00010000, 0x00030001, 0xFFFE0000, 0x00000001); // halfword LSBs 1 0 1 1 0 0 0 1
    m.exec(as::gbh(3, 1));
    CHECK(gw(m.r(3), 0) == 0xB1u);
    m.r(1) = bytes({1, 0, 0, 0, 0, 0, 0, 1, 3, 2, 2, 2, 2, 2, 2, 0xFF});
    m.exec(as::gbb(3, 1));
    CHECK(gw(m.r(3), 0) == 0x8181u);
}
