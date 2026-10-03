// Interpreter unit tests: branches (BO/BI decoding, CTR, LR), loads/stores (update, algebraic, byte-reversed,
// multiple, string), reservations, dcbz, sc/trap hooks, timebase.

#include <catch_amalgamated.hpp>

#include "ppu_test_util.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::ppu;
using namespace pspocket::ppu::test;
namespace as = pspocket::recomp::ppu::as;

TEST_CASE("b / bl / ba / bla targets and LR")
{
    Machine m;
    const u32 pc = Machine::kCode + 0x100;
    m.exec(as::b_rel(0x40), pc);
    CHECK(m.ctx.cia == pc + 0x40);
    m.ctx.lr = 0;
    m.exec(as::b_rel(-0x100, true), pc);
    CHECK(m.ctx.cia == pc - 0x100);
    CHECK(m.ctx.lr == pc + 4);
    m.exec(as::b_abs(0x1230, true), pc);
    CHECK(m.ctx.cia == 0x1230);
    CHECK(m.ctx.lr == pc + 4);
}

TEST_CASE("bc: every BO class, CTR decrement and 64-bit CTR test")
{
    Machine m;
    const u32 pc = Machine::kCode + 0x200;
    const u32 target = pc + 0x80;
    struct Row {
        u32 bo;
        u64 ctr_in;
        u8 cr_bit;
        bool taken;
        u64 ctr_out;
    };
    const Row rows[] = {
        {0, 2, 0, true, 1},     // dec, CTR != 0 and cond false
        {0, 2, 1, false, 1},
        {0, 1, 0, false, 0},
        {2, 1, 0, true, 0},     // dec, CTR == 0 and cond false
        {2, 2, 0, false, 1},
        {4, 9, 0, true, 9},     // cond false, no dec
        {4, 9, 1, false, 9},
        {8, 2, 1, true, 1},     // dec, CTR != 0 and cond true
        {8, 2, 0, false, 1},
        {10, 1, 1, true, 0},    // dec, CTR == 0 and cond true
        {12, 5, 1, true, 5},    // cond true
        {12, 5, 0, false, 5},
        {16, 3, 0, true, 2},    // bdnz
        {16, 1, 1, false, 0},
        {18, 1, 0, true, 0},    // bdz
        {20, 7, 0, true, 7},    // always
        {16, 0, 0, true, ~0ull}, // CTR wraps: 0 - 1 != 0
        {16, 0x100000001ull, 0, true, 0x100000000ull}, // the full 64-bit CTR is tested
    };
    for (const Row& r : rows) {
        INFO("BO " << r.bo << " ctr " << r.ctr_in << " cr " << int(r.cr_bit));
        m.ctx.ctr = r.ctr_in;
        m.ctx.cr[6] = r.cr_bit;
        m.exec(as::bc_rel(r.bo, 6, 0x80), pc);
        CHECK(m.ctx.cia == (r.taken ? target : pc + 4));
        CHECK(m.ctx.ctr == r.ctr_out);
    }
    // bcl sets LR whether or not the branch is taken
    m.ctx.lr = 0;
    m.ctx.cr[6] = 0;
    m.exec(as::bc_rel(12, 6, 0x80, true), pc);
    CHECK(m.ctx.cia == pc + 4);
    CHECK(m.ctx.lr == pc + 4);
}

TEST_CASE("bclr / bcctr: targets from LR/CTR (low 2 bits cleared), LK updates LR after the read")
{
    Machine m;
    const u32 pc = Machine::kCode + 0x300;
    m.ctx.lr = 0xFFFFFFFF00012347ull;
    m.exec(as::blr(), pc);
    CHECK(m.ctx.cia == 0x00012344);
    m.ctx.lr = 0x00015000;
    m.exec(as::blrl(), pc);
    CHECK(m.ctx.cia == 0x00015000);
    CHECK(m.ctx.lr == pc + 4);
    // bdnzlr
    m.ctx.lr = 0x2000;
    m.ctx.ctr = 1;
    m.exec(as::bclr(16, 0), pc);
    CHECK(m.ctx.cia == pc + 4);
    CHECK(m.ctx.ctr == 0);
    // beqlr not taken / taken
    m.ctx.cr[2] = 0;
    m.exec(as::bclr(12, 2), pc);
    CHECK(m.ctx.cia == pc + 4);
    m.ctx.cr[2] = 1;
    m.exec(as::bclr(12, 2), pc);
    CHECK(m.ctx.cia == 0x2000);

    m.ctx.ctr = 0x00016003;
    m.exec(as::bctr(), pc);
    CHECK(m.ctx.cia == 0x00016000);
    CHECK(m.ctx.ctr == 0x00016003); // no decrement
    m.exec(as::bctrl(), pc);
    CHECK(m.ctx.lr == pc + 4);
    m.ctx.cr[1] = 0;
    m.exec(as::bcctr(4, 1), pc); // bnectr-style: branch if cr0.gt false
    CHECK(m.ctx.cia == 0x00016000);
}

TEST_CASE("loads: zero/sign extension, update forms, (rA|0), indexed, byte-reversed")
{
    Machine m;
    const u32 d = Machine::kData;
    m.w64(d, 0x8182838485868788ull);
    m.w64(d + 8, 0x0102030405060708ull);
    m.ctx.gpr[4] = d;
    m.exec(as::lbz(3, 1, 4));
    CHECK(m.ctx.gpr[3] == 0x82);
    m.exec(as::lhz(3, 0, 4));
    CHECK(m.ctx.gpr[3] == 0x8182);
    m.exec(as::lha(3, 0, 4));
    CHECK(m.ctx.gpr[3] == 0xFFFFFFFFFFFF8182ull);
    m.exec(as::lwz(3, 4, 4));
    CHECK(m.ctx.gpr[3] == 0x85868788);
    m.exec(as::lwa(3, 4, 4));
    CHECK(m.ctx.gpr[3] == 0xFFFFFFFF85868788ull);
    m.exec(as::ld(3, 8, 4));
    CHECK(m.ctx.gpr[3] == 0x0102030405060708ull);

    // update: rA receives the full 64-bit EA; the load uses its low 32 bits
    m.ctx.gpr[4] = 0xABCD000000000000ull | d;
    m.exec(as::lwzu(3, 8, 4));
    CHECK(m.ctx.gpr[3] == 0x01020304);
    CHECK(m.ctx.gpr[4] == (0xABCD000000000000ull | (d + 8)));

    // rA = 0 means literal 0 (not r0)
    m.ctx.gpr[0] = 0x12345678;
    REQUIRE(m.mem.commit(0, 0x2000));
    m.w32(0x1000, 0xCAFEBABE);
    m.exec(as::lwz(3, 0x1000, 0));
    CHECK(m.ctx.gpr[3] == 0xCAFEBABE);
    m.ctx.gpr[5] = 0x1000;
    m.exec(as::lwzx(3, 0, 5));
    CHECK(m.ctx.gpr[3] == 0xCAFEBABE);

    m.ctx.gpr[4] = d;
    m.ctx.gpr[5] = 8;
    m.exec(as::lhax(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 0x0102);
    m.exec(as::lwbrx(3, 4, 5));
    CHECK(m.ctx.gpr[3] == 0x04030201);
    m.exec(as::x_mem(Op::LHBRX, 3, 4, 5));
    CHECK(m.ctx.gpr[3] == 0x0201);
    m.exec(as::x_mem(Op::LDBRX, 3, 4, 5));
    CHECK(m.ctx.gpr[3] == 0x0807060504030201ull);
    m.exec(as::x_mem(Op::LHAUX, 3, 4, 5)); // sign-extend 0x0102, update r4
    CHECK(m.ctx.gpr[3] == 0x0102);
    CHECK(m.ctx.gpr[4] == d + 8);
}

TEST_CASE("stores: widths, update forms, byte-reversed")
{
    Machine m;
    const u32 d = Machine::kData;
    m.ctx.gpr[3] = 0x1122334455667788ull;
    m.ctx.gpr[4] = d;
    m.exec(as::stb(3, 0, 4));
    m.exec(as::sth(3, 2, 4));
    m.exec(as::stw(3, 4, 4));
    CHECK(m.r64(d) == 0x8800778855667788ull);
    m.exec(as::std_(3, 8, 4));
    CHECK(m.r64(d + 8) == 0x1122334455667788ull);
    m.exec(as::stdu(3, 16, 4));
    CHECK(m.ctx.gpr[4] == d + 16);
    CHECK(m.r64(d + 16) == 0x1122334455667788ull);
    m.ctx.gpr[5] = 8;
    m.exec(as::stwbrx(3, 4, 5));
    CHECK(m.r32(d + 24) == 0x88776655);
    m.exec(as::x_mem(Op::STDBRX, 3, 4, 5));
    CHECK(m.r64(d + 24) == 0x8877665544332211ull);
    m.exec(as::x_mem(Op::STHBRX, 3, 4, 5));
    CHECK(m.r32(d + 24) == 0x88776655); // the first halfword rewritten with the same bytes
    // stwu with rS == rA stores the old rA
    m.ctx.gpr[4] = d + 0x40;
    m.exec(as::stwu(4, 8, 4));
    CHECK(m.r32(d + 0x48) == d + 0x40);
    CHECK(m.ctx.gpr[4] == d + 0x48);
}

TEST_CASE("lmw / stmw move words to/from rT..r31")
{
    Machine m;
    const u32 d = Machine::kData;
    m.ctx.gpr[29] = 0xAAAAAAAA11111111ull;
    m.ctx.gpr[30] = 0x22222222;
    m.ctx.gpr[31] = 0xFFFFFFFF33333333ull;
    m.ctx.gpr[3] = d;
    m.exec(as::stmw(29, 0, 3));
    CHECK(m.r32(d) == 0x11111111);
    CHECK(m.r32(d + 4) == 0x22222222);
    CHECK(m.r32(d + 8) == 0x33333333);
    m.ctx.gpr[30] = m.ctx.gpr[31] = 0;
    m.exec(as::lmw(30, 4, 3));
    CHECK(m.ctx.gpr[30] == 0x22222222);
    CHECK(m.ctx.gpr[31] == 0x33333333); // zero-extended
}

TEST_CASE("string loads/stores: lswi/stswi/lswx/stswx (partial last register, wrap to r0)")
{
    Machine m;
    const u32 d = Machine::kData;
    for (u32 i = 0; i < 40; i++) m.w8(d + i, static_cast<u8>(0x10 + i));
    m.ctx.gpr[4] = d;
    m.exec(as::lswi(5, 4, 7));
    CHECK(m.ctx.gpr[5] == 0x10111213);
    CHECK(m.ctx.gpr[6] == 0x14151600); // last register: bytes in the high end, rest zero

    m.ctx.gpr[9] = d;
    m.exec(as::lswi(30, 9, 0)); // NB 0 = 32 bytes: r30, r31, r0..r5
    CHECK(m.ctx.gpr[30] == 0x10111213);
    CHECK(m.ctx.gpr[31] == 0x14151617);
    CHECK(m.ctx.gpr[0] == 0x18191A1B);
    CHECK(m.ctx.gpr[3] == 0x24252627);

    m.ctx.gpr[7] = 0xAABBCCDDEEFF0011ull;
    m.ctx.gpr[8] = 0x99887766;
    m.ctx.gpr[9] = d + 0x100;
    m.exec(as::stswi(7, 9, 6));
    CHECK(m.r32(d + 0x100) == 0xEEFF0011);
    CHECK(m.r8(d + 0x104) == 0x99);
    CHECK(m.r8(d + 0x105) == 0x88);
    CHECK(m.r8(d + 0x106) == 0x00); // untouched (zero memory)

    m.ctx.gpr[4] = d; // reloaded: the 32-byte lswi above wrote r4
    m.ctx.xer_bc = 5;
    m.ctx.gpr[10] = 2;
    m.exec(as::lswx(11, 4, 10)); // EA = d + 2, 5 bytes
    CHECK(m.ctx.gpr[11] == 0x12131415);
    CHECK(m.ctx.gpr[12] == 0x16000000);
    m.ctx.gpr[10] = 0x200;
    m.exec(as::stswx(11, 4, 10));
    CHECK(m.r32(d + 0x200) == 0x12131415);
    CHECK(m.r8(d + 0x204) == 0x16);
    m.ctx.xer_bc = 0; // zero bytes: nothing happens
    m.ctx.gpr[11] = 77;
    m.exec(as::lswx(11, 4, 10));
    CHECK(m.ctx.gpr[11] == 77);
}

TEST_CASE("lwarx/stwcx. and ldarx/stdcx.: single-thread reservation model")
{
    Machine m;
    const u32 d = Machine::kData;
    m.w32(d, 100);
    m.ctx.gpr[3] = d;
    m.ctx.gpr[4] = 0;
    m.exec(as::lwarx(5, 4, 3));
    CHECK(m.ctx.gpr[5] == 100);
    m.ctx.gpr[6] = 101;
    m.exec(as::stwcx_(6, 4, 3));
    CHECK(m.crf(0) == EQ);
    CHECK(m.r32(d) == 101);
    // The reservation is consumed: a second stwcx. fails and does not store.
    m.ctx.gpr[6] = 999;
    m.exec(as::stwcx_(6, 4, 3));
    CHECK(m.crf(0) == 0);
    CHECK(m.r32(d) == 101);
    // A store that changes the reserved word in between loses the reservation (data compare, as RPCS3).
    m.exec(as::lwarx(5, 4, 3));
    m.w32(d, 5);
    m.exec(as::stwcx_(6, 4, 3));
    CHECK(m.crf(0) == 0);
    CHECK(m.r32(d) == 5);
    // Different address
    m.exec(as::lwarx(5, 4, 3));
    m.ctx.gpr[7] = d + 4;
    m.exec(as::stwcx_(6, 4, 7));
    CHECK(m.crf(0) == 0);
    // SO is copied into CR0
    m.ctx.xer_so = 1;
    m.w64(d + 8, 0x1111);
    m.ctx.gpr[8] = d + 8;
    m.exec(as::ldarx(5, 4, 8));
    m.ctx.gpr[6] = 0x2222;
    m.exec(as::stdcx_(6, 4, 8));
    CHECK(m.crf(0) == (EQ | SO));
    CHECK(m.r64(d + 8) == 0x2222);
    // lwarx then stdcx. (size mismatch) fails
    m.exec(as::lwarx(5, 4, 8));
    m.exec(as::stdcx_(6, 4, 8));
    CHECK(m.crf(0) == SO);
}

TEST_CASE("dcbz clears the whole 128-byte line")
{
    Machine m;
    const u32 d = Machine::kData + 0x400;
    for (u32 i = 0; i < 0x200; i += 4) m.w32(d - 0x80 + i, 0xFFFFFFFF);
    m.ctx.gpr[3] = d;
    m.ctx.gpr[4] = 0x45;
    m.exec(as::dcbz(3, 4));
    for (u32 i = 0; i < 128; i += 4) CHECK(m.r32(d + i) == 0);
    CHECK(m.r32(d - 4) == 0xFFFFFFFF);
    CHECK(m.r32(d + 128) == 0xFFFFFFFF);
}

namespace {
u64 g_syscalls = 0;
u32 g_trap_pc = 0;
void count_syscall(PPUContext* ctx, u8*)
{
    g_syscalls++;
    ctx->gpr[3] = ctx->gpr[11] * 2 + ctx->cia;
}
void note_trap(PPUContext*, u8*, u32 pc) { g_trap_pc = pc; }
u64 fixed_tb() { return 0x0000001234567890ull; }
} // namespace

TEST_CASE("sc calls the syscall hook with the sc address; traps call the trap hook")
{
    Machine m;
    ps_ppu_set_syscall_handler(count_syscall);
    ps_ppu_set_trap_handler(note_trap);
    const u32 pc = Machine::kCode + 0x40;
    m.ctx.gpr[11] = 21;
    m.exec(as::sc(), pc);
    CHECK(g_syscalls == 1);
    CHECK(m.ctx.gpr[3] == 42 + pc);
    CHECK(m.ctx.cia == pc + 4);

    m.ctx.gpr[3] = static_cast<u64>(-1);
    m.ctx.gpr[4] = 1;
    g_trap_pc = 0;
    m.exec(as::tw(0x10, 3, 4), pc); // lt: -1 < 1 traps
    CHECK(g_trap_pc == pc);
    CHECK(m.ctx.cia == pc + 4);
    g_trap_pc = 0;
    m.exec(as::tw(0x01, 3, 4), pc); // gtu: 0xFFFFFFFF > 1 traps
    CHECK(g_trap_pc == pc);
    g_trap_pc = 0;
    m.exec(as::tw(0x08, 3, 4), pc); // gt: no trap
    CHECK(g_trap_pc == 0);
    m.ctx.gpr[3] = 0x100000000ull;
    m.exec(as::tdi(0x04, 3, 0), pc); // eq as 64-bit: no trap
    CHECK(g_trap_pc == 0);
    m.exec(as::twi(0x04, 3, 0), pc); // eq as 32-bit: traps
    CHECK(g_trap_pc == pc);
    ps_ppu_set_syscall_handler(nullptr);
    ps_ppu_set_trap_handler(nullptr);
}

TEST_CASE("mftb / mftbu / mfspr TBL read the timebase source")
{
    Machine m;
    ps_ppu_set_timebase_source(fixed_tb);
    m.exec(as::mftb(3));
    CHECK(m.ctx.gpr[3] == 0x0000001234567890ull);
    m.exec(as::mftb(3, kSprTbu));
    CHECK(m.ctx.gpr[3] == 0x12);
    m.exec(as::mfspr(4, kSprTbl));
    CHECK(m.ctx.gpr[4] == 0x0000001234567890ull);
    ps_ppu_set_timebase_source(nullptr);
    // The default source is the running 79.8 MHz clock.
    m.exec(as::mftb(3));
    const u64 t0 = m.ctx.gpr[3];
    m.exec(as::mftb(3));
    CHECK(m.ctx.gpr[3] >= t0);
}
