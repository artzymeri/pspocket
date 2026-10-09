// Raw SPUs and interrupt threads (D-528): a synthetic SPU program in a raw SPU's local store, driven through its
// problem-state registers (guest loads and stores that fault into the MMIO emulator), its mailboxes, its stop status,
// the MFC proxy, a class-2 mailbox interrupt served by an interrupt thread, and the calls' errors.

#include <csignal>

#include "lv2_test_util.hpp"
#include "pspocket/core/mmio.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;
namespace as = pspocket::recomp::spu::as;

namespace {

constexpr u32 kSpuInitialize = 169, kRawCreate = 160, kRawDestroy = 161, kCreateTag = 150, kSetMask = 151,
              kGetMask = 152, kSetStat = 153, kGetStat = 154, kReadPuint = 163, kSetCfg = 196, kGetCfg = 197,
              kTagDestroy = 81, kEstablish = 84, kEoi = 88, kDisestablish = 89;
constexpr u32 kLs = 0xE0000000, kProb = kLs + 0x40000;
constexpr u32 MFC_LSA = 0x3004, MFC_EAH = 0x3008, MFC_EAL = 0x300C, MFC_Size_Tag = 0x3010, MFC_CMD = 0x3014,
              MFC_QStatus = 0x3104, Out_MBox = 0x4004, In_MBox = 0x400C, MBox_Status = 0x4014, RunCntl = 0x401C,
              Status = 0x4024, NPC = 0x4034;

struct sigaction g_old_segv, g_old_bus;

void on_fault(int sig, siginfo_t* info, void* uctx)
{
    if (mmio_handle_fault(info->si_addr, uctx)) return;
    sigaction(sig, sig == SIGBUS ? &g_old_bus : &g_old_segv, nullptr);
    raise(sig);
}

struct FaultHandler {
    FaultHandler()
    {
        struct sigaction sa{};
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &g_old_segv);
        sigaction(SIGBUS, &sa, &g_old_bus);
    }
    ~FaultHandler()
    {
        sigaction(SIGSEGV, &g_old_segv, nullptr);
        sigaction(SIGBUS, &g_old_bus, nullptr);
    }
};

struct InterpretSpu {
    ps_spu_miss_policy saved = ps_spu_get_miss_policy();
    InterpretSpu() { ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET); }
    ~InterpretSpu() { ps_spu_set_miss_policy(saved); }
};

template <class Pred>
bool wait_until(Pred pred)
{
    for (int i = 0; i < 2000; i++) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

} // namespace

TEST_CASE("lv2 raw SPU: problem state, mailboxes, MFC proxy and an interrupt thread", "[lv2][raw-spu]")
{
    if (!mmio_supported()) SKIP("raw SPU registers need fault emulation (Apple AArch64)");
    InterpretSpu interpret;
    FaultHandler handler;
    HostEntries entries;
    Fixture f(entries.config());
    Caller c(*f.kernel, "ppu");
    u8* b = f.base();
    const u32 out = f.alloc(8);

    CHECK(c.call(kRawCreate, out, 0) == rc(CELL_EAGAIN)); // no raw SPU in the share yet
    REQUIRE(c.call(kSpuInitialize, 6, 1) == CELL_OK);
    REQUIRE(c.call(kRawCreate, out, 0) == CELL_OK);
    const u32 id = f.r32(out);
    CHECK(id == 0);
    CHECK(c.call(kRawCreate, out, 0) == rc(CELL_EAGAIN)); // the share is one
    CHECK(f.mem.is_committed(kLs + 0x3FFFF));
    CHECK(!f.mem.is_committed(kProb));

    // The program: $3 = the inbound mailbox; +1 to the outbound one, +2 to the interrupt mailbox; stop 0x1234.
    const std::vector<u32> code = {
        as::rdch(3, PS_SPU_RdInMbox), as::ai(3, 3, 1),  as::wrch(PS_SPU_WrOutMbox, 3), as::ai(3, 3, 1),
        as::wrch(PS_SPU_WrOutIntrMbox, 3), as::stop(0x1234),
    };
    for (u32 i = 0; i < code.size(); i++) ps_write32(b, kLs + 4 * i, code[i]);
    ps_write32(b, kLs + 0x100, as::br(0)); // a spin, for the stop request

    // Through the registers: everything below is a guest load or store that faults into the emulator.
    CHECK(ps_read32(b, kProb + Status) == 0);
    CHECK(ps_read32(b, kProb + MBox_Status) == (4u << 8)); // four free inbound entries
    ps_write32(b, kProb + In_MBox, 41);
    CHECK(ps_read32(b, kProb + MBox_Status) == (3u << 8));
    ps_write32(b, kProb + NPC, 0);
    ps_write32(b, kProb + RunCntl, 1);
    REQUIRE(wait_until([&] { return (ps_read32(b, kProb + Status) & 1) == 0; }));
    CHECK(ps_read32(b, kProb + Status) == ((0x1234u << 16) | 2)); // stopped by stop, its code above
    CHECK(ps_read32(b, kProb + MBox_Status) == (1u | (4u << 8) | (1u << 16)));
    CHECK(ps_read32(b, kProb + Out_MBox) == 42);
    CHECK(ps_read32(b, kProb + MBox_Status) == ((4u << 8) | (1u << 16)));
    CHECK(ps_read32(b, kProb + NPC) == 4 * code.size()); // the instruction after the stop
    CHECK(ps_read32(b, kProb + RunCntl) == 1);
    REQUIRE(c.call(kGetStat, id, 2, out) == CELL_OK);
    CHECK(f.r64(out) == 3); // the mailbox and stop-and-signal interrupts, none masked in
    REQUIRE(c.call(kReadPuint, id, out) == CELL_OK);
    CHECK(f.r32(out) == 43);
    REQUIRE(c.call(kSetStat, id, 2, 3) == CELL_OK);
    REQUIRE(c.call(kGetStat, id, 2, out) == CELL_OK);
    CHECK(f.r64(out) == 0);

    // The errors.
    CHECK(c.call(kGetStat, id, 1, out) == rc(CELL_EINVAL));
    CHECK(c.call(kGetStat, 3, 2, out) == rc(CELL_ESRCH));
    CHECK(c.call(kSetCfg, id, 4) == rc(CELL_EINVAL));
    REQUIRE(c.call(kSetCfg, id, 3) == CELL_OK);
    REQUIRE(c.call(kGetCfg, id, out) == CELL_OK);
    CHECK(f.r32(out) == 3);
    REQUIRE(c.call(kSetCfg, id, 0) == CELL_OK);

    // A class-2 tag and an interrupt thread whose handler takes the interrupt mailbox.
    REQUIRE(c.call(kCreateTag, id, 2, 0, out) == CELL_OK);
    const u32 tag = f.r32(out);
    CHECK((tag & 0xFF000000) == 0x0A000000);
    CHECK(c.call(kCreateTag, id, 2, 0, out) == rc(CELL_EAGAIN));
    std::atomic<u32> calls{0}, got{0};
    std::atomic<u64> args{0};
    const u32 hbuf = f.alloc(8);
    entries.add(0x5000, [&](PPUContext& ctx) {
        args = (ctx.gpr[3] << 32) | ctx.gpr[4];
        syscall(ctx, kReadPuint, id, hbuf);
        got = f.r32(hbuf);
        syscall(ctx, kSetStat, id, 2, 1);
        calls++;
        syscall(ctx, kEoi); // back to the wait: does not return
        FAIL("sys_interrupt_thread_eoi returned");
    });
    const u32 opd = f.alloc(8), param = f.alloc(8), tid = f.alloc(8), name = f.cstr("intr");
    f.w32(opd, 0x5000);
    f.w32(opd + 4, 0x6000);
    f.w32(param, opd);
    f.w32(param + 4, 0x7000); // TLS
    REQUIRE(c.call(kThreadCreate, tid, param, 0, 0, 100, 0x4000, 2, name) == CELL_OK);
    const u32 thread = static_cast<u32>(f.r64(tid));
    CHECK(c.call(kEstablish, out, 0x0A00FFFF, thread, 0, 0) == rc(CELL_ESRCH));
    REQUIRE(c.call(kEstablish, out, tag, thread, 0xAA, 0xBB) == CELL_OK);
    const u32 ih = f.r32(out);
    CHECK((ih & 0xFF000000) == 0x0B000000);
    CHECK(c.call(kEstablish, out, tag, thread, 0, 0) == rc(CELL_EAGAIN)); // already running
    CHECK(c.call(kTagDestroy, tag) == rc(CELL_EBUSY));
    REQUIRE(c.call(kSetMask, id, 2, 1) == CELL_OK);
    REQUIRE(c.call(kGetMask, id, 2, out) == CELL_OK);
    CHECK(f.r64(out) == 1);

    ps_write32(b, kProb + In_MBox, 100);
    ps_write32(b, kProb + NPC, 0);
    ps_write32(b, kProb + RunCntl, 1);
    REQUIRE(wait_until([&] { return calls.load() == 1; }));
    CHECK(got == 102);
    CHECK(args == ((0xAAull << 32) | 0xBB));
    REQUIRE(wait_until([&] { return (ps_read32(b, kProb + Status) & 1) == 0; }));
    CHECK(ps_read32(b, kProb + Out_MBox) == 101);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(calls == 1); // the stop's interrupt is masked out

    REQUIRE(c.call(kDisestablish, ih, out) == CELL_OK);
    CHECK(f.r64(out) == 0x7000); // the thread's r13
    CHECK(c.call(kDisestablish, ih, out) == rc(CELL_ESRCH));
    REQUIRE(c.call(kTagDestroy, tag) == CELL_OK);
    CHECK(c.call(kTagDestroy, tag) == rc(CELL_ESRCH));

    // A stop request: the write returns once the spinning SPU has stopped, its NPC where it was.
    ps_write32(b, kProb + NPC, 0x100);
    ps_write32(b, kProb + RunCntl, 1);
    CHECK((ps_read32(b, kProb + Status) & 1) == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ps_write32(b, kProb + RunCntl, 0);
    CHECK((ps_read32(b, kProb + Status) & 1) == 0);
    CHECK(ps_read32(b, kProb + NPC) == 0x100);

    // The MFC proxy: a PUT from the local store, a GET into it, and a command status read without its writes.
    const u32 dst = f.alloc(32);
    for (u32 i = 0; i < 4; i++) ps_write32(b, kLs + 0x2000 + 4 * i, 0xC0DE0000 + i);
    CHECK(ps_read32(b, kProb + MFC_QStatus) == 0x80000008);
    ps_write32(b, kProb + MFC_LSA, 0x2000);
    ps_write32(b, kProb + MFC_EAH, 0);
    ps_write32(b, kProb + MFC_EAL, dst);
    ps_write32(b, kProb + MFC_Size_Tag, (16u << 16) | 1);
    ps_write32(b, kProb + MFC_CMD, 0x20); // PUT
    CHECK(ps_read32(b, kProb + MFC_CMD) == 0);
    CHECK(f.r32(dst + 12) == 0xC0DE0003);
    CHECK(ps_read32(b, kProb + MFC_CMD) == 1); // a sequence error: nothing written since
    f.w32(dst + 16, 0xFEEDFACE);
    ps_write32(b, kProb + MFC_LSA, 0x3000);
    ps_write32(b, kProb + MFC_EAH, 0);
    ps_write32(b, kProb + MFC_EAL, dst + 16);
    ps_write32(b, kProb + MFC_Size_Tag, (16u << 16) | 1);
    ps_write32(b, kProb + MFC_CMD, 0x40); // GET
    CHECK(ps_read32(b, kProb + MFC_CMD) == 0);
    CHECK(ps_read32(b, kLs + 0x3000) == 0xFEEDFACE);

    REQUIRE(c.call(kRawDestroy, id) == CELL_OK);
    CHECK(c.call(kRawDestroy, id) == rc(CELL_ESRCH));
    CHECK(!f.mem.is_committed(kLs));
    REQUIRE(c.call(kRawCreate, out, 0) == CELL_OK); // its index is free again
    CHECK(f.r32(out) == 0);
}

TEST_CASE("lv2 interrupt threads: a never-established thread disestablished, isolated SPUs' errors", "[lv2][raw-spu]")
{
    HostEntries entries;
    Fixture f(entries.config());
    Caller c(*f.kernel, "ppu");
    const u32 out = f.alloc(8);
    const u32 opd = f.alloc(8), param = f.alloc(8), tid = f.alloc(8);
    f.w32(opd, 0x5000);
    f.w32(param, opd);
    f.w32(param + 4, 0x7100);
    REQUIRE(c.call(kThreadCreate, tid, param, 0, 0, 100, 0x4000, 2, 0) == CELL_OK);
    const u32 thread = static_cast<u32>(f.r64(tid));
    REQUIRE(c.call(kDisestablish, thread, out) == CELL_OK); // RPCS3: the unestablished thread ends, its r13 given
    CHECK(f.r64(out) == 0x7100);
    CHECK(c.call(kDisestablish, thread, out) == rc(CELL_ESRCH));
    CHECK(c.call(80) == CELL_OK); // sys_interrupt_tag_create: RPCS3's null slot
    CHECK(c.call(kTagDestroy, 0x0A000001) == rc(CELL_ESRCH));
    CHECK(c.call(232, 0) == rc(CELL_ESRCH)); // sys_isolated_spu_start without one
    CHECK(c.call(237, 0, 2, out) == rc(CELL_ESRCH));
    CHECK(c.call(237, 0, 1, out) == rc(CELL_EINVAL));
    // Not an SPU ELF: CELL_EAUTHFAIL.
    const u32 img = f.alloc(0x100);
    CHECK(c.call(230, out, img, 1, 2, 3, 4) == rc(CELL_EAUTHFAIL));
}
