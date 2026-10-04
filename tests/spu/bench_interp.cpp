// SPU interpreter speed (hidden: `test_spu "[.bench]"`, not run by ctest). A loop of typical job code (quadword loads
// and stores, float multiply-add, shuffles and rotates, integer and compare ops, a branch), 14 instructions an
// iteration, run by ps_spu_interpret; prints millions of instructions per second. For before/after numbers when the
// interpreter changes (CLAUDE.md rule 8).

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstdio>

#include "pspocket/recomp/spu/asm.hpp"
#include "spu_test_util.hpp"

using namespace pspocket;
using namespace pspocket::spu;
using namespace pspocket::spu::test;
namespace as = pspocket::recomp::spu::as;

TEST_CASE("bench: SPU interpreter, a loop of typical job instructions", "[.bench]")
{
    constexpr u32 kIterations = 2'000'000, kPerIteration = 14;
    as::Assembler a(0x100);
    a.emit(as::ilhu(10, static_cast<s32>(kIterations >> 16)));
    a.emit(as::iohl(10, static_cast<s32>(kIterations & 0xFFFF)));
    a.emit(as::il(11, 0x2000)); // a buffer in LS
    a.emit(as::il(26, 0x0F0E));  // a shuffle pattern
    const auto loop = a.here();
    a.emit(as::lqd(20, 0, 11));
    a.emit(as::lqd(21, 16, 11));
    a.emit(as::fa(22, 20, 21));
    a.emit(as::fm(23, 22, 21));
    a.emit(as::fma(24, 23, 20, 22));
    a.emit(as::shufb(25, 20, 21, 26));
    a.emit(as::rotqby(27, 25, 10));
    a.emit(as::a(28, 27, 10));
    a.emit(as::xor_(29, 28, 20));
    a.emit(as::cgt(30, 29, 21));
    a.emit(as::and_(31, 30, 29));
    a.emit(as::stqd(31, 32, 11));
    a.emit(as::ai(10, 10, -1));
    a.brnz(10, loop);
    a.emit(as::stop(0x1234));

    Machine m;
    m.load(0x100, a.finish());
    m.ctx.pc = 0x100;
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(ps_spu_interpret(&m.ctx, 0) == PS_SPU_STOP_SIGNAL);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double mips = static_cast<double>(kIterations) * kPerIteration / s / 1e6;
    std::printf("SPU interpreter: %.1f MIPS (%u instructions in %.3f s)\n", mips, kIterations * kPerIteration, s);
    CHECK(m.ctx.stop_code == 0x1234u);
}
