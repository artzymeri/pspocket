// Hand-checked interpreter vectors: loads and stores (LS wrap, quadword masking), every branch form, interrupt
// enable bits, stop/stopd, halts, illegal instructions, special registers, and short programs run by the run loop.

#include <catch_amalgamated.hpp>

#include "pspocket/recomp/spu/asm.hpp"
#include "spu_test_util.hpp"

using namespace pspocket;
using namespace pspocket::spu;
using namespace pspocket::spu::test;
namespace as = pspocket::recomp::spu::as;

namespace {

void fill_ls(Machine& m, u32 addr)
{
    for (u32 i = 0; i < 16; i++) m.ls.data()[addr + i] = static_cast<u8>(0x40 + i);
}

} // namespace

TEST_CASE("quadword loads: d, x, a, r forms, alignment masking and LS wrap")
{
    Machine m;
    fill_ls(m, 0x2000);
    m.r(1) = words(0x2005, 0x99999999, 0, 0); // low 4 bits are ignored
    m.exec(as::lqd(3, 0, 1));
    CHECK(hex(m.r(3)) == "40414243 44454647 48494a4b 4c4d4e4f");
    m.r(1) = words(0x2000 - 0x20, 0, 0, 0);
    m.exec(as::lqd(3, 32, 1));
    CHECK(gw(m.r(3), 0) == 0x40414243u);
    m.r(1) = words(0x3FFF0 + 0x10, 0, 0, 0); // 0x40000 wraps to 0
    fill_ls(m, 0);
    m.exec(as::lqd(3, 0, 1));
    CHECK(gw(m.r(3), 0) == 0x40414243u);
    m.r(1) = words(0x1000, 0, 0, 0);
    m.r(2) = words(0x100F, 0, 0, 0);
    m.exec(as::lqx(3, 1, 2)); // 0x200F & ~15
    CHECK(gw(m.r(3), 3) == 0x4C4D4E4Fu);
    m.exec(as::lqa(3, 0x2000));
    CHECK(gw(m.r(3), 1) == 0x44454647u);
    m.exec(as::lqr(3, 0x1000), 0x1000); // pc 0x1000 + 0x1000
    CHECK(gw(m.r(3), 2) == 0x48494A4Bu);
    m.exec(as::lqr(3, -0x2000), 0x2008); // (0x2008 - 0x2000) & ~15 = 0
    CHECK(gw(m.r(3), 0) == 0x40414243u);
}

TEST_CASE("quadword stores write big-endian bytes")
{
    Machine m;
    m.r(7) = words(0x00112233, 0x44556677, 0x8899AABB, 0xCCDDEEFF);
    m.r(1) = words(0x3000, 0, 0, 0);
    m.exec(as::stqd(7, -16, 1)); // 0x2FF0
    for (u32 i = 0; i < 16; i++) CHECK(m.ls.data()[0x2FF0 + i] == static_cast<u8>(i * 0x11));
    m.r(2) = words(0x3FFF8, 0, 0, 0);
    m.r(3) = words(0x8, 0, 0, 0);
    m.exec(as::stqx(7, 2, 3)); // 0x40000 -> 0
    CHECK(m.ls.data()[0] == 0x00);
    CHECK(m.ls.data()[15] == 0xFF);
    m.exec(as::stqa(7, 0x3FFF0));
    CHECK(m.ls.data()[0x3FFF3] == 0x33);
    m.exec(as::stqr(7, 0x20), 0x100);
    CHECK(m.ls.data()[0x120 + 4] == 0x44);
}

TEST_CASE("direct branches, calls and the link register")
{
    Machine m;
    m.exec(as::br(-8), 0x100);
    CHECK(m.ctx.pc == 0xF8u);
    m.exec(as::br(-8), 0x4); // wraps below 0
    CHECK(m.ctx.pc == 0x3FFFCu);
    m.exec(as::bra(0x1230));
    CHECK(m.ctx.pc == 0x1230u);
    m.exec(as::brsl(0, 0x40), 0x200);
    CHECK(m.ctx.pc == 0x240u);
    CHECK(hex(m.r(0)) == "00000204 00000000 00000000 00000000");
    m.exec(as::brasl(5, 0x3000), 0x3FFFC); // link wraps to 0
    CHECK(m.ctx.pc == 0x3000u);
    CHECK(gw(m.r(5), 0) == 0u);

    // conditional branches test the preferred word / halfword only
    m.r(3) = words(0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
    m.exec(as::brz(3, 0x10), 0x100);
    CHECK(m.ctx.pc == 0x110u);
    m.exec(as::brnz(3, 0x10), 0x100);
    CHECK(m.ctx.pc == 0x104u);
    m.r(3) = words(0xFFFF0000, 0, 0, 0); // preferred halfword (bits 16-31) is zero
    m.exec(as::brhz(3, 0x10), 0x100);
    CHECK(m.ctx.pc == 0x110u);
    m.exec(as::brhnz(3, 0x10), 0x100);
    CHECK(m.ctx.pc == 0x104u);
    m.exec(as::brz(3, 0x10), 0x100);
    CHECK(m.ctx.pc == 0x104u);
}

TEST_CASE("indirect branches, iret, bisled and the interrupt bits")
{
    Machine m;
    m.r(4) = words(0x12347, 0, 0, 0); // target masked to 0x12344
    m.exec(as::bi(4), 0x100);
    CHECK(m.ctx.pc == 0x12344u);
    m.exec(as::bisl(4, 4), 0x100); // rt == ra: the target is read first
    CHECK(m.ctx.pc == 0x12344u);
    CHECK(gw(m.r(4), 0) == 0x104u);
    m.r(4) = words(0x500, 0, 0, 0);
    m.r(3) = words(1, 0, 0, 0);
    m.exec(as::biz(3, 4), 0x100);
    CHECK(m.ctx.pc == 0x104u);
    m.exec(as::binz(3, 4), 0x100);
    CHECK(m.ctx.pc == 0x500u);
    m.exec(as::bihz(3, 4), 0x100);
    CHECK(m.ctx.pc == 0x104u);
    m.exec(as::bihnz(3, 4), 0x100);
    CHECK(m.ctx.pc == 0x500u);
    m.ctx.srr0 = 0x777;
    m.exec(as::iret(0), 0x100);
    CHECK(m.ctx.pc == 0x774u);

    // bisled: link always written; branch only when an enabled event is pending
    m.exec(as::bisled(5, 4), 0x100);
    CHECK(m.ctx.pc == 0x104u);
    CHECK(gw(m.r(5), 0) == 0x104u);
    m.ctx.ch.event_mask = PS_SPU_EVENT_S1;
    ps_spu_write_signal(&m.ctx, 1, 0x55);
    m.exec(as::bisled(5, 4), 0x100);
    CHECK(m.ctx.pc == 0x500u);

    // E/D bits on a taken branch
    m.ctx.ch.event_mask = 0;
    m.ctx.ch.event_stat = 0;
    m.ctx.ch.event_count = 0;
    m.exec(as::bi(4, true, false), 0x100);
    CHECK(m.ctx.interrupts_enabled == 1);
    m.exec(as::bi(4, false, true), 0x100);
    CHECK(m.ctx.interrupts_enabled == 0);
    // enabling interrupts with an enabled event pending takes the interrupt: SRR0 = target, pc = 0
    m.ctx.ch.event_mask = PS_SPU_EVENT_S1;
    ps_spu_write_signal(&m.ctx, 1, 0x66);
    m.exec(as::bi(4, true, false), 0x100);
    CHECK(m.ctx.pc == 0u);
    CHECK(m.ctx.srr0 == 0x500u);
    CHECK(m.ctx.interrupts_enabled == 0);
}

TEST_CASE("stop, stopd, halts, illegal instructions")
{
    Machine m;
    CHECK_FALSE(m.exec(as::stop(0x2102), 0x200));
    CHECK(m.ctx.stop_reason == PS_SPU_STOP_SIGNAL);
    CHECK(m.ctx.stop_code == 0x2102u);
    CHECK(m.ctx.pc == 0x204u);
    CHECK_FALSE(m.exec(as::stopd(0, 0, 0), 0x200));
    CHECK(m.ctx.stop_code == 0x3FFFu);

    m.r(1) = words(5, 0, 0, 0);
    m.r(2) = words(5, 1, 1, 1);
    CHECK(m.exec(as::hgt(0, 1, 2), 0x300));
    CHECK(m.ctx.pc == 0x304u);
    CHECK_FALSE(m.exec(as::heq(0, 1, 2), 0x300));
    CHECK(m.ctx.stop_reason == PS_SPU_STOP_HALT);
    CHECK(m.ctx.pc == 0x300u);
    m.r(1) = words(0xFFFFFFFF, 0, 0, 0);
    CHECK(m.exec(as::hgti(0, 1, 0), 0x300)); // -1 > 0 is false
    CHECK_FALSE(m.exec(as::hlgti(0, 1, 0), 0x300));

    CHECK_FALSE(m.exec(0x00800000u, 0x400)); // unassigned opcode
    CHECK(m.ctx.stop_reason == PS_SPU_STOP_EXCEPTION);
    CHECK(m.ctx.exception == PS_SPU_EXC_ILLEGAL);
    CHECK(m.ctx.stop_code == 0x00800000u);
    CHECK(m.ctx.pc == 0x400u);
    CHECK_FALSE(m.exec(as::dfceq(1, 2, 3), 0x400));
    CHECK(m.ctx.exception == PS_SPU_EXC_ILLEGAL);
    CHECK_FALSE(m.exec(as::bi(4, true, true), 0x400));
}

namespace {
u32 g_global_stops = 0, g_own_stops = 0;
int count_global_stop(SPUContext*, u32) { return ++g_global_stops, 1; } // handled: continue
int count_own_stop(SPUContext*, u32 code) { return ++g_own_stops, code == 0x111; } // 0x111 continues, others stop
} // namespace

TEST_CASE("per-context hooks take a context's stop codes before the process-wide handler")
{
    g_global_stops = g_own_stops = 0;
    ps_spu_set_stop_handler(count_global_stop);
    Machine m;
    CHECK(m.exec(as::stop(0x2102), 0x200)); // no hooks: the process-wide handler continues it
    CHECK(g_global_stops == 1);
    const ps_spu_hooks hooks{count_own_stop, nullptr};
    m.ctx.hooks = &hooks;
    CHECK(m.exec(as::stop(0x111), 0x200)); // the context's handler continues it
    CHECK_FALSE(m.exec(as::stop(0x222), 0x200)); // ... or stops it
    CHECK(m.ctx.stop_reason == PS_SPU_STOP_SIGNAL);
    CHECK(m.ctx.stop_code == 0x222u);
    CHECK(g_own_stops == 2);
    CHECK(g_global_stops == 1); // never consulted for this context
    const ps_spu_hooks no_stop{nullptr, nullptr};
    m.ctx.hooks = &no_stop;
    CHECK(m.exec(as::stop(0x2102), 0x200)); // a NULL member falls back to the process-wide handler
    CHECK(g_global_stops == 2);
    ps_spu_set_stop_handler(nullptr);
}

TEST_CASE("no-ops and special registers")
{
    Machine m;
    m.r(3) = splat(0x12345678);
    for (u32 insn : {as::nop(3), as::lnop(), as::sync(), as::dsync(), as::hbr(8, 3), as::hbra(8, 0x100), as::hbrr(8, 0x40),
             as::mtspr(5, 3)}) {
        CHECK(m.exec(insn, 0x100));
        CHECK(m.ctx.pc == 0x104u);
    }
    CHECK(gw(m.r(3), 0) == 0x12345678u);
    m.exec(as::mfspr(3, 5)); // every SPR reads as zero
    CHECK(hex(m.r(3)) == "00000000 00000000 00000000 00000000");
}

namespace {
SPUContext* g_seen_current = nullptr;
int note_current(SPUContext*, uint32_t code)
{
    g_seen_current = ps_spu_current();
    return code == 0x10; // continue after the first stop, stop at the second
}
} // namespace

// ps_spu_current names the context a host thread is running (pspocket-boot's fault report reads it): set inside
// ps_spu_run, through the stop hooks it calls, and cleared when the run returns.
TEST_CASE("run loop: ps_spu_current is the running context, and null outside a run")
{
    as::Assembler a(0x100);
    a.emit(as::stop(0x10));
    a.emit(as::stop(0x20));
    Machine m;
    m.load(0x100, a.finish());
    const ps_spu_hooks hooks{note_current, nullptr};
    m.ctx.hooks = &hooks;
    m.ctx.pc = 0x100;
    CHECK(ps_spu_current() == nullptr);
    g_seen_current = nullptr;
    CHECK(ps_spu_run(&m.ctx) == PS_SPU_STOP_SIGNAL);
    CHECK(m.ctx.stop_code == 0x20u);
    CHECK(g_seen_current == &m.ctx);
    CHECK(ps_spu_current() == nullptr);
    m.ctx.hooks = nullptr;
}

TEST_CASE("run loop: a loop with a call, executed by ps_spu_interpret")
{
    // sum = 0; for (i = 10; i != 0; i--) sum += double(i); stop 0x1234
    as::Assembler a(0x100);
    a.emit(as::il(3, 0));  // sum
    a.emit(as::il(4, 10)); // i
    auto loop = a.here();
    auto dbl = a.label();
    a.brsl(0, dbl);
    a.emit(as::a(3, 3, 5));
    a.emit(as::ai(4, 4, -1));
    a.brnz(4, loop);
    a.emit(as::stop(0x1234));
    a.bind(dbl);
    a.emit(as::a(5, 4, 4));
    a.emit(as::bi(0));
    Machine m;
    m.load(0x100, a.finish());
    m.ctx.pc = 0x100;
    CHECK(ps_spu_interpret(&m.ctx, 0) == PS_SPU_STOP_SIGNAL);
    CHECK(m.ctx.stop_code == 0x1234u);
    CHECK(gw(m.r(3), 0) == 110u);
    CHECK(m.ctx.pc == a.address(dbl)); // the instruction after the stop

    m.ctx.pc = 0x100;
    CHECK(ps_spu_interpret(&m.ctx, 5) == PS_SPU_STOP_LIMIT);
}
