// SPURS semaphores (CellSpursSemaphore) as libsre has them (runtime/hle/src/cellSpursSemaphore.cpp):
// _cellSpursSemaphoreInitialize's checks in libsre's order and the line it writes, for a taskset's semaphore and for
// one of the whole SPURS instance; then synthetic SPU tasks (assembled here: no game data) that take and give units
// with P and V the way the SPU-side semaphore library does, on the native scheduler with several SPUs. There are more
// tasks than units, so tasks wait and are woken by the V of a task on another SPU: after they have switched out, before
// their syscall 2, and from another taskset (through the workload signal the SPU code sets).

#include "hle_test_util.hpp"

#include <chrono>
#include <cstring>
#include <format>
#include <functional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "pspocket/core/reservation.h"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/spu/context.h"
#include "guest.hpp" // runtime/hle/src: reservation_op

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::spu::as;
using namespace std::chrono_literals;

namespace {

constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kTsAttr = kData + 0x4000,
              kTsAttr2 = kData + 0x4200, kPrio = kData + 0x4400;
constexpr u32 kTs = kData + 0x6000, kTs2 = kData + 0x8000; // 6400 bytes each
constexpr u32 kSem = kData + 0xA000, kSem2 = kData + 0xA080, kFakeTs = kData + 0xA200;
// Shared by the tasks: the stats line (inside, the most inside at once, entries, tasks done) and, in the next line,
// the PPU's controls (the gate the first holders wait for, the pause in P and its gate).
constexpr u32 kStats = kData + 0xA400, kControl = kStats + 0x80;
constexpr u32 kId = kData + 0xA600, kArg = kData + 0xA700, kPattern = kData + 0xA800;
constexpr u32 kReports = kData + 0xB000; // 32 bytes per task: entries, waits; the pause marker at +16
constexpr u32 kElf = kData + 0xC000;
constexpr u32 kCtx = kData + 0x20000; // 0xC00 bytes per task: 1 KB + one LS block (the stack's)
constexpr u32 kCtxSize = 0xC00;

const u64 kInval = r3_of_code(0x80410902), kFault = r3_of_code(0x8041090D), kAlign = r3_of_code(0x80410910),
          kNull = r3_of_code(0x80410911);

bool wait_for(const std::function<bool()>& done, std::chrono::milliseconds timeout = 5000ms)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// A consistent copy of a guest line (the tasks change it with PUTLLC).
struct Line {
    alignas(16) u8 bytes[PS_RES_LINE_SIZE];
    u32 word(u32 off) const
    {
        return (u32{bytes[off]} << 24) | (u32{bytes[off + 1]} << 16) | (u32{bytes[off + 2]} << 8) | bytes[off + 3];
    }
    u8 byte(u32 off) const { return bytes[off]; }
};
Line snapshot(u8* b, u32 addr)
{
    Line l;
    ps_res_acquire(b, addr, l.bytes);
    return l;
}

// A control word stored with one reservation operation, as the PPU's stwcx. would: the tasks poll the line with
// GETLLAR, and the version it moves orders what the test did before (its checks of a saved context) before what the
// tasks do once they see it (D-585).
void set_control(u8* b, u32 offset, u32 value)
{
    hle::reservation_op(b, kControl, [&](u8* line) {
        ps_write32(line, offset, value);
        return true;
    });
}

// One SPURS instance with `spus` SPUs and one or two tasksets (priority 1 everywhere, max contention `spus`). The
// native scheduler's workers run only when asked: the PPU API test needs none.
struct SemFixture {
    Fixture f;
    GuestThread a{f, 0, "ppu"};
    u8* b = f.base();

    static hle::ProcessConfig config(bool workers)
    {
        hle::ProcessConfig c;
        c.spurs_workers = workers;
        return c;
    }

    explicit SemFixture(bool workers = false, u32 spus = 1, u32 tasksets = 1) : f(config(workers))
    {
        REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, spus, 200, 2016, 0) == 0);
        REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
        std::memset(b + kPrio, 1, 8);
        REQUIRE(a.call("_cellSpursTasksetAttributeInitialize", kTsAttr, 1, 0x330000, 0x1234, kPrio, spus) == 0);
        REQUIRE(a.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs, kTsAttr) == 0);
        if (tasksets > 1) {
            REQUIRE(a.call("_cellSpursTasksetAttributeInitialize", kTsAttr2, 1, 0x330000, 0x5678, kPrio, spus) == 0);
            REQUIRE(a.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs2, kTsAttr2) == 0);
        }
        std::memset(b + kPattern, 0, 16);
        ps_write8(b, kPattern + 15, 1); // the stack's block (0x3F800..0x3FFFF): enough to wait and yield
    }
    ~SemFixture() { CHECK(a.call("cellSpursFinalize", kSpurs) == 0); }

    u32 wid(u32 taskset) const { return ps_read32(b, taskset + 0x74); }
    // The bit sets change with the workers' reservation operations: one copy of the line, so a context the worker saved
    // before a task's waiting bit is visible with the bit (D-585).
    bool task_bit(u32 taskset, u32 set_offset, u32 id) const
    {
        return line_word(b, taskset + set_offset + 4 * (id / 32)) & (0x80000000u >> (id % 32));
    }
    // Creates a task running the ELF at kElf with {semaphore, iterations, stats, report}; returns its ID.
    u32 create(u32 taskset, u32 semaphore, u32 iterations, u32 report, u32 context)
    {
        ps_write32(b, kArg + 0, semaphore);
        ps_write32(b, kArg + 4, iterations);
        ps_write32(b, kArg + 8, kStats);
        ps_write32(b, kArg + 12, report);
        REQUIRE(a.call("cellSpursCreateTask", taskset, kId, kElf, context, kCtxSize, kPattern, kArg) == 0);
        return ps_read32(b, kId);
    }
};

// ------------------------------------------------------------------------------------------------- the SPU task

constexpr u32 kBuf = 0x3E000;     // the task's DMA buffer (outside its LS pattern: used only between syscalls)
constexpr u32 kLine = 0x80;       // GETLLAR/PUTLLC buffer below the kernel context, where SPU-side libraries keep it
constexpr u32 kSpursLine = 0x100; // CellSpurs line 0 goes through the kernel context's copy, as the library does

// Registers that live across syscalls ($80..$127, which the taskset policy saves and restores).
enum : u32 {
    rSem = 80, rLeft = 81, rStats = 82, rReport = 83, rWaits = 84, rHold = 85, rLink = 86, rControl = 87,
    rEntries = 88, rLink2 = 89, rWaiter = 90, rWaiterWid = 91, rWaiterTs = 92, rNeedSignal = 93, rTsWid = 94,
    rCellSpurs = 95,
};

// GETLLAR (status discarded) or PUTLLC (`status` non-zero when it failed) of the line at EA register `ea`.
void line_cmd(as::Assembler& a, u32 cmd, u32 ea, u32 ls, u32 status)
{
    constexpr u32 t = 70;
    a.emit(as::ila(t, ls));
    a.emit(as::wrch(PS_MFC_LSA, t));
    a.emit(as::il(t, 0));
    a.emit(as::wrch(PS_MFC_EAH, t));
    a.emit(as::wrch(PS_MFC_EAL, ea));
    a.emit(as::il(t, 128));
    a.emit(as::wrch(PS_MFC_Size, t));
    a.emit(as::il(t, static_cast<s32>(cmd)));
    a.emit(as::wrch(PS_MFC_Cmd, t));
    a.emit(as::rdch(status, PS_MFC_RdAtomicStat));
}
void getllar(as::Assembler& a, u32 ea, u32 ls) { line_cmd(a, PS_MFC_GETLLAR, ea, ls, 71); }
void putllc(as::Assembler& a, u32 ea, u32 ls, u32 status) { line_cmd(a, PS_MFC_PUTLLC, ea, ls, status); }

// A DMA of `size` bytes between LS `ls` and the EA in register `ea`, waited for (tag 1).
void dma(as::Assembler& a, u32 cmd, u32 ls, u32 ea, u32 size)
{
    constexpr u32 t = 70;
    a.emit(as::ila(t, ls));
    a.emit(as::wrch(PS_MFC_LSA, t));
    a.emit(as::il(t, 0));
    a.emit(as::wrch(PS_MFC_EAH, t));
    a.emit(as::wrch(PS_MFC_EAL, ea));
    a.emit(as::il(t, static_cast<s32>(size)));
    a.emit(as::wrch(PS_MFC_Size, t));
    a.emit(as::il(t, 1));
    a.emit(as::wrch(PS_MFC_TagID, t));
    a.emit(as::il(t, static_cast<s32>(cmd)));
    a.emit(as::wrch(PS_MFC_Cmd, t));
    a.emit(as::il(t, 2));
    a.emit(as::wrch(PS_MFC_WrTagMask, t));
    a.emit(as::il(t, PS_MFC_TAG_UPDATE_ALL));
    a.emit(as::wrch(PS_MFC_WrTagUpdate, t));
    a.emit(as::rdch(t, PS_MFC_RdTagStat));
}

// The taskset policy's syscall, as the task library calls it: through the entry the taskset context holds (0x27C4).
void syscall(as::Assembler& a, u32 n)
{
    a.emit(as::lqa(5, 0x27C0));
    a.emit(as::rotqbyi(5, 5, 4));
    a.emit(as::il(3, static_cast<s32>(n)));
    a.emit(as::bisl(0, 5));
}

// rt = qw with its word (byte) at `offset` replaced by `value`'s preferred word (byte). $1 (the stack, 16-aligned)
// is the base the masks are generated from.
void insert_word(as::Assembler& a, u32 rt, u32 value, u32 qw, u32 offset)
{
    a.emit(as::cwd(72, 1, static_cast<s32>(offset)));
    a.emit(as::shufb(rt, value, qw, 72));
}
void insert_byte(as::Assembler& a, u32 rt, u32 value, u32 qw, u32 offset)
{
    a.emit(as::cbd(72, 1, static_cast<s32>(offset)));
    a.emit(as::shufb(rt, value, qw, 72));
}

// The test task (written for this test from the protocol in cellSpursSemaphore.cpp). Argument: {semaphore, iterations,
// stats EA, report EA}. Each iteration takes a unit (P), counts itself in (inside, the most inside at once, entries),
// yields while it holds the unit (so tasks on its SPU find none and wait), counts itself out and gives the unit back
// (V). In its first iteration it stays in until the PPU opens the gate (control word 0), yielding meanwhile. With
// control word 1 set, a P that has to wait first marks its report (+16) and stays, running, until control word 2 is
// set. At the end: the report {entries, waits}, done + 1, exit.
std::vector<u8> semaphore_task()
{
    as::Assembler a(0x3000);
    const auto P = a.label(), V = a.label(), signal = a.label(), enter = a.label(), leave = a.label(),
               finished = a.label(), loop = a.label(), hold = a.label(), held = a.label(), finish = a.label();

    a.emit(as::ila(1, 0x3FFD0));
    a.emit(as::ori(rSem, 3, 0));
    a.emit(as::rotqbyi(rLeft, 3, 4));
    a.emit(as::rotqbyi(rStats, 3, 8));
    a.emit(as::rotqbyi(rReport, 3, 12));
    a.emit(as::ai(rControl, rStats, 0x80));
    a.emit(as::il(rWaits, 0));
    a.emit(as::il(rHold, 1));
    a.emit(as::il(rEntries, 0));
    a.bind(loop);
    a.brz(rLeft, finish);
    a.brsl(rLink, P);
    a.brsl(rLink, enter);
    a.brz(rHold, held);
    a.bind(hold);
    // The PPU's control words, polled with GETLLAR: a lock-line read, as SPU code waits on a flag a PPU stores, reads
    // the line atomically, where a plain GET's copy raced with the PPU's store (D-585).
    getllar(a, rControl, kBuf);
    a.emit(as::lqa(10, kBuf));
    a.brnz(10, held);
    syscall(a, 1);
    a.br(hold);
    a.bind(held);
    a.emit(as::il(rHold, 0));
    syscall(a, 1);
    a.brsl(rLink, leave);
    a.brsl(rLink, V);
    a.emit(as::ai(rLeft, rLeft, -1));
    a.emit(as::ai(rEntries, rEntries, 1));
    a.br(loop);
    a.bind(finish);
    a.emit(as::il(10, 0));
    insert_word(a, 11, rEntries, 10, 0);
    insert_word(a, 11, rWaits, 11, 4);
    a.emit(as::stqa(11, kBuf));
    dma(a, PS_MFC_PUT, kBuf, rReport, 16);
    a.brsl(rLink, finished);
    syscall(a, 0);

    // P: one atomic update of the semaphore line. A unit (count > 0): count - 1. None: append the task ID (0x11 + n,
    // n at 0x10) and workload ID (0x21 + n, n at 0x20), then wait for a signal with the wait object semaphore | 1. The
    // V that signals hands over its unit: no second look at the count.
    const auto p_retry = a.label(), p_wait = a.label(), p_pause = a.label(), p_sleep = a.label(), p_bad = a.label();
    a.bind(P);
    a.bind(p_retry);
    getllar(a, rSem, kLine);
    a.emit(as::lqa(10, kLine)); // count: word 0
    a.emit(as::cgti(11, 10, 0));
    a.brz(11, p_wait);
    a.emit(as::ai(12, 10, -1));
    insert_word(a, 13, 12, 10, 0);
    a.emit(as::stqa(13, kLine));
    putllc(a, rSem, kLine, 15);
    a.brnz(15, p_retry);
    a.emit(as::bi(rLink));
    a.bind(p_wait);
    a.emit(as::il(22, 0));
    a.emit(as::lqa(16, kLine + 0x10));
    a.emit(as::rotqbyi(17, 16, 13));
    a.emit(as::andi(17, 17, 0xFF)); // tasks waiting
    a.emit(as::clgti(18, 17, 14));
    a.brnz(18, p_bad);              // 15 wait already (the library's AGAIN; never in these tests)
    a.emit(as::lqa(19, 0x27D0));
    a.emit(as::rotqbyi(19, 19, 4)); // the task ID (0x27D4)
    a.emit(as::ai(20, 17, 1));
    a.emit(as::cbx(21, 20, 22));
    a.emit(as::shufb(16, 19, 16, 21));
    insert_byte(a, 16, 20, 16, 0);
    a.emit(as::stqa(16, kLine + 0x10));
    a.emit(as::lqa(24, kLine + 0x20));
    a.emit(as::rotqbyi(25, 24, 13));
    a.emit(as::andi(25, 25, 0xFF));
    a.emit(as::lqa(26, 0x1D0));
    a.emit(as::rotqbyi(26, 26, 12)); // the workload ID (0x1DC)
    a.emit(as::ai(27, 25, 1));
    a.emit(as::cbx(21, 27, 22));
    a.emit(as::shufb(24, 26, 24, 21));
    insert_byte(a, 24, 27, 24, 0);
    a.emit(as::stqa(24, kLine + 0x20));
    putllc(a, rSem, kLine, 15);
    a.brnz(15, p_retry);
    getllar(a, rControl, kBuf); // the control words, as above
    a.emit(as::lqa(10, kBuf));
    a.emit(as::rotqbyi(11, 10, 4));
    a.brz(11, p_sleep);              // no pause
    a.emit(as::il(12, 1));
    a.emit(as::stqa(12, kBuf));
    a.emit(as::ai(13, rReport, 16));
    dma(a, PS_MFC_PUT, kBuf, 13, 16);
    a.bind(p_pause);
    getllar(a, rControl, kBuf); // the control words, as above
    a.emit(as::lqa(10, kBuf));
    a.emit(as::rotqbyi(11, 10, 8));
    a.brz(11, p_pause);
    a.bind(p_sleep);
    a.emit(as::il(22, 0));
    a.emit(as::lqa(28, 0x2FD0));
    a.emit(as::ori(29, rSem, 1));
    insert_word(a, 28, 22, 28, 8); // the wait object (u64 at 0x2FD8)
    insert_word(a, 28, 29, 28, 12);
    a.emit(as::stqa(28, 0x2FD0));
    syscall(a, 2);
    a.emit(as::ai(rWaits, rWaits, 1));
    a.emit(as::il(22, 0));
    a.emit(as::lqa(28, 0x2FD0));
    insert_word(a, 28, 22, 28, 12);
    a.emit(as::stqa(28, 0x2FD0));
    a.brnz(3, p_bad); // the wait failed
    a.emit(as::bi(rLink));
    a.bind(p_bad);
    a.emit(as::stop(0x101));

    // V: one atomic update. Count >= 0 and tasks waiting: the oldest leaves the lists (the others move up), the count
    // stays, and it gets a signal. Otherwise count + 1.
    const auto v_retry = a.label(), v_count = a.label(), v_send = a.label();
    a.bind(V);
    a.bind(v_retry);
    getllar(a, rSem, kLine);
    a.emit(as::lqa(10, kLine));
    a.emit(as::cgti(11, 10, -1));
    a.brz(11, v_count);
    a.emit(as::lqa(16, kLine + 0x10));
    a.emit(as::rotqbyi(17, 16, 13));
    a.emit(as::andi(17, 17, 0xFF));
    a.brz(17, v_count); // nobody waits
    a.emit(as::rotqbyi(rWaiter, 16, 14));
    a.emit(as::andi(rWaiter, rWaiter, 0xFF)); // the oldest (0x11)
    a.emit(as::shlqbyi(18, 16, 1));
    a.emit(as::ai(19, 17, -1));
    insert_byte(a, 18, 19, 18, 0);
    a.emit(as::stqa(18, kLine + 0x10));
    a.emit(as::lqa(24, kLine + 0x20));
    a.emit(as::rotqbyi(25, 24, 13));
    a.emit(as::andi(25, 25, 0xFF));
    a.emit(as::rotqbyi(rWaiterWid, 24, 14));
    a.emit(as::andi(rWaiterWid, rWaiterWid, 0xFF));
    a.emit(as::shlqbyi(26, 24, 1));
    a.emit(as::ai(27, 25, -1));
    insert_byte(a, 26, 27, 26, 0);
    a.emit(as::stqa(26, kLine + 0x20));
    putllc(a, rSem, kLine, 15);
    a.brnz(15, v_retry);
    a.emit(as::lqa(28, kLine + 0x30));
    a.emit(as::rotqbyi(rWaiterTs, 28, 12)); // the semaphore's taskset (0x38, low word)
    a.brnz(rWaiterTs, v_send);
    // A semaphore of the whole instance: the waiter's workload's taskset, WorkloadInfo.arg of the CellSpurs at 0x1C0.
    a.emit(as::lqa(29, 0x1C0));
    a.emit(as::rotqbyi(29, 29, 4));
    a.emit(as::shli(30, rWaiterWid, 5));
    a.emit(as::a(30, 30, 29));
    a.emit(as::ila(31, 0xB00));
    a.emit(as::a(30, 30, 31));
    dma(a, PS_MFC_GET, kBuf, 30, 16);
    a.emit(as::lqa(32, kBuf));
    a.emit(as::rotqbyi(rWaiterTs, 32, 12));
    a.bind(v_send);
    a.brsl(rLink2, signal);
    a.emit(as::bi(rLink));
    a.bind(v_count);
    a.emit(as::ai(12, 10, 1));
    insert_word(a, 13, 12, 10, 0);
    a.emit(as::stqa(13, kLine));
    putllc(a, rSem, kLine, 15);
    a.brnz(15, v_retry);
    a.emit(as::bi(rLink));

    // _cellSpursSendSignal from SPU code: the task's signalled bit in its taskset's line; when it waits and was not
    // signalled yet, its workload's bit in wklSignal1 (the upper half of CellSpurs' word at 0x70; workloads 0..15).
    const auto s_retry = a.label(), w_retry = a.label(), s_done = a.label();
    a.bind(signal);
    a.bind(s_retry);
    getllar(a, rWaiterTs, kLine);
    a.emit(as::rotmi(40, rWaiter, -5));
    a.emit(as::shli(40, 40, 2)); // the task's word in a 128-bit set
    a.emit(as::andi(41, rWaiter, 31));
    a.emit(as::ilhu(42, 0x8000));
    a.emit(as::sfi(43, 41, 0));
    a.emit(as::rotm(42, 42, 43)); // its bit
    a.emit(as::lqa(44, kLine + 0x40));
    a.emit(as::lqa(45, kLine + 0x50));
    a.emit(as::rotqby(46, 44, 40)); // signalled
    a.emit(as::rotqby(47, 45, 40)); // waiting
    a.emit(as::andc(48, 47, 46));
    a.emit(as::and_(rNeedSignal, 48, 42));
    a.emit(as::or_(46, 46, 42));
    a.emit(as::il(49, 0));
    a.emit(as::cwx(50, 40, 49));
    a.emit(as::shufb(44, 46, 44, 50));
    a.emit(as::stqa(44, kLine + 0x40));
    a.emit(as::lqa(51, kLine + 0x70));
    a.emit(as::rotqbyi(rTsWid, 51, 4)); // the taskset's workload ID (0x74)
    putllc(a, rWaiterTs, kLine, 15);
    a.brnz(15, s_retry);
    a.brz(rNeedSignal, s_done);
    a.emit(as::lqa(52, 0x1C0));
    a.emit(as::rotqbyi(rCellSpurs, 52, 4));
    a.bind(w_retry);
    getllar(a, rCellSpurs, kSpursLine);
    a.emit(as::ilhu(53, 0x8000));
    a.emit(as::sfi(54, rTsWid, 0));
    a.emit(as::rotm(53, 53, 54));
    a.emit(as::fsmbi(55, 0xF000));
    a.emit(as::and_(53, 53, 55));
    a.emit(as::lqa(56, kSpursLine + 0x70));
    a.emit(as::or_(56, 56, 53));
    a.emit(as::stqa(56, kSpursLine + 0x70));
    putllc(a, rCellSpurs, kSpursLine, 15);
    a.brnz(15, w_retry);
    a.bind(s_done);
    a.emit(as::bi(rLink2));

    // The stats line: inside + 1, the most inside at once, entries + 1 (enter); inside - 1 (leave); done + 1.
    const auto e_retry = a.label(), l_retry = a.label(), f_retry = a.label();
    a.bind(enter);
    a.bind(e_retry);
    getllar(a, rStats, kLine);
    a.emit(as::lqa(10, kLine));
    a.emit(as::ai(11, 10, 1));
    a.emit(as::rotqbyi(12, 10, 4));
    a.emit(as::cgt(13, 11, 12));
    a.emit(as::selb(14, 12, 11, 13));
    a.emit(as::rotqbyi(15, 10, 8));
    a.emit(as::ai(15, 15, 1));
    insert_word(a, 16, 11, 10, 0);
    insert_word(a, 16, 14, 16, 4);
    insert_word(a, 16, 15, 16, 8);
    a.emit(as::stqa(16, kLine));
    putllc(a, rStats, kLine, 17);
    a.brnz(17, e_retry);
    a.emit(as::bi(rLink));
    a.bind(leave);
    a.bind(l_retry);
    getllar(a, rStats, kLine);
    a.emit(as::lqa(10, kLine));
    a.emit(as::ai(11, 10, -1));
    insert_word(a, 16, 11, 10, 0);
    a.emit(as::stqa(16, kLine));
    putllc(a, rStats, kLine, 17);
    a.brnz(17, l_retry);
    a.emit(as::bi(rLink));
    a.bind(finished);
    a.bind(f_retry);
    getllar(a, rStats, kLine);
    a.emit(as::lqa(10, kLine));
    a.emit(as::ai(11, 10, 1));
    a.emit(as::rotqbyi(11, 11, 12));
    insert_word(a, 16, 11, 10, 12);
    a.emit(as::stqa(16, kLine));
    putllc(a, rStats, kLine, 17);
    a.brnz(17, f_retry);
    a.emit(as::bi(rLink));

    return a.finish();
}

// A task ELF at `ea`: one read-only PT_LOAD at 0x3000 with the program (what libsre's loader accepts).
void write_task_elf(u8* base, u32 ea, const std::vector<u8>& code)
{
    const u32 size = static_cast<u32>(code.size());
    REQUIRE(0x3000 + size <= kBuf);
    std::memset(base + ea, 0, 0x100 + size + 16);
    static constexpr u8 kIdent[16] = {0x7F, 'E', 'L', 'F', 1, 2, 1, 0};
    std::memcpy(base + ea, kIdent, 16);
    ps_write16(base, ea + 0x10, 2);    // ET_EXEC
    ps_write16(base, ea + 0x12, 0x17); // EM_SPU
    ps_write32(base, ea + 0x14, 1);
    ps_write32(base, ea + 0x18, 0x3000); // entry
    ps_write32(base, ea + 0x1C, 0x34);   // phoff
    ps_write16(base, ea + 0x28, 0x34);
    ps_write16(base, ea + 0x2A, 0x20);
    ps_write16(base, ea + 0x2C, 1);
    const u32 ph = ea + 0x34;
    ps_write32(base, ph + 0x00, 1);      // PT_LOAD
    ps_write32(base, ph + 0x04, 0x100);  // offset
    ps_write32(base, ph + 0x08, 0x3000); // vaddr
    ps_write32(base, ph + 0x0C, 0x3000); // paddr
    ps_write32(base, ph + 0x10, size);
    ps_write32(base, ph + 0x14, (size + 15) & ~15u);
    ps_write32(base, ph + 0x18, 5); // R X
    ps_write32(base, ph + 0x1C, 0x80);
    std::memcpy(base + ea + 0x100, code.data(), size);
}

// What a stalled run shows: the semaphore, the stats and the tasksets' task bits.
void dump_state(u8* b)
{
    for (const u32 addr : {kSem, kStats, kTs, kTs2}) {
        for (u32 off = 0; off < 0x80; off += 0x10) {
            UNSCOPED_INFO(std::format("{:#x} +{:#04x}: {:08x} {:08x} {:08x} {:08x}", addr, off,
                ps_read32(b, addr + off), ps_read32(b, addr + off + 4), ps_read32(b, addr + off + 8),
                ps_read32(b, addr + off + 12)));
        }
    }
}

} // namespace

TEST_CASE("SPURS semaphores: _cellSpursSemaphoreInitialize as libsre, checks in its order",
    "[hle][spurs][spurs-semaphore]")
{
    SemFixture s;
    u8* b = s.b;
    auto init = [&](u32 spurs, u32 taskset, u32 semaphore, u32 total) {
        return s.a.call("_cellSpursSemaphoreInitialize", spurs, taskset, semaphore, total);
    };
    auto zero_except_fields = [&](u32 semaphore) {
        for (u32 off = 4; off < 0x30; off++) CHECK(b[semaphore + off] == 0);
        for (u32 off = 0x40; off < 0x80; off++) CHECK(b[semaphore + off] == 0);
    };

    // GTA V's call: no SPURS, its taskset, 4 units. The whole line is written; reservations on it are broken.
    std::memset(b + kSem, 0xAA, 128);
    const u64 version = ps_res_version(kSem);
    REQUIRE(init(0, kTs, kSem, 4) == 0);
    CHECK(ps_res_version(kSem) != version);
    CHECK(ps_read32(b, kSem + 0x00) == 4);
    CHECK(ps_read64(b, kSem + 0x30) == 0); // SPURS as passed
    CHECK(ps_read64(b, kSem + 0x38) == kTs);
    zero_except_fields(kSem);

    // A semaphore of the whole instance (cellSpursSemaphoreInitializeIWL): CellSpurs at 0x30, no taskset.
    std::memset(b + kSem2, 0xAA, 128);
    REQUIRE(init(kSpurs, 0, kSem2, 2) == 0);
    CHECK(ps_read32(b, kSem2 + 0x00) == 2);
    CHECK(ps_read64(b, kSem2 + 0x30) == kSpurs);
    CHECK(ps_read64(b, kSem2 + 0x38) == 0);
    zero_except_fields(kSem2);
    // With SPURS, the taskset is only stored, whatever it is: misaligned, not guest memory, or not a taskset.
    REQUIRE(init(kSpurs, kTs + 0x40, kSem2, 2) == 0);
    CHECK(ps_read64(b, kSem2 + 0x38) == kTs + 0x40);
    REQUIRE(init(kSpurs, 0x40000000, kSem2, 2) == 0);
    CHECK(ps_read64(b, kSem2 + 0x38) == 0x40000000);
    std::memset(b + kFakeTs, 0, 0x80);
    ps_write32(b, kFakeTs + 0x74, 32); // workload ID 32
    REQUIRE(init(kSpurs, kFakeTs, kSem2, 2) == 0);
    // The total is not checked.
    REQUIRE(init(0, kTs, kSem2, 0xFFFFFFFB) == 0);
    CHECK(ps_read32(b, kSem2) == 0xFFFFFFFB);
    REQUIRE(init(0, kTs, kSem2, 0x7FFFFFFF) == 0);
    CHECK(ps_read32(b, kSem2) == 0x7FFFFFFF);
    REQUIRE(init(0, kTs, kSem2, 0) == 0);
    CHECK(ps_read32(b, kSem2) == 0);

    // With SPURS: semaphore NULL, then SPURS and semaphore alignment.
    CHECK(init(kSpurs, kTs, 0, 1) == kNull);
    CHECK(init(kSpurs + 8, kTs, 0, 1) == kNull);
    CHECK(init(kSpurs + 8, kTs, kSem, 1) == kAlign);
    CHECK(init(kSpurs + 0x40, 0, kSem + 0x40, 1) == kAlign);
    CHECK(init(kSpurs, kTs, kSem + 0x40, 1) == kAlign);
    // Without: taskset or semaphore NULL, taskset then semaphore alignment, then the taskset's workload ID (above 31:
    // INVAL; 31 is accepted, registered or not).
    CHECK(init(0, 0, kSem, 1) == kNull);
    CHECK(init(0, 0, 0, 1) == kNull);
    CHECK(init(0, kTs, 0, 1) == kNull);
    CHECK(init(0, kTs + 0x40, 0, 1) == kNull);
    CHECK(init(0, kTs + 0x40, kSem, 1) == kAlign);
    CHECK(init(0, kTs, kSem + 0x10, 1) == kAlign);
    CHECK(init(0, kFakeTs, kSem, 1) == kInval);
    CHECK(init(0, kFakeTs, kSem + 0x40, 1) == kAlign);
    ps_write32(b, kFakeTs + 0x74, 31);
    REQUIRE(init(0, kFakeTs, kSem2, 1) == 0);
    CHECK(ps_read64(b, kSem2 + 0x38) == kFakeTs);
    ps_write32(b, kFakeTs + 0x74, 32);
    // Pointers libsre would just use: refused instead (D-042), where it would first touch them.
    CHECK(init(0, 0x40000000, kSem, 1) == kFault);
    CHECK(init(0, 0x40000000, kSem + 0x40, 1) == kAlign); // alignment first
    CHECK(init(0, kTs, 0x40000000, 1) == kFault);
    CHECK(init(0, kFakeTs, 0x40000000, 1) == kInval); // the taskset is looked at before the semaphore is touched
    CHECK(init(kSpurs, 0, 0x40000000, 1) == kFault);
    // None of the refused calls wrote anything.
    CHECK(ps_read32(b, kSem + 0x00) == 4);
    CHECK(ps_read64(b, kSem + 0x38) == kTs);
    zero_except_fields(kSem);
}

TEST_CASE("SPURS semaphores: SPU tasks on four SPUs take and give units; waiters are woken by another SPU's V",
    "[hle][spurs][spurs-semaphore]")
{
    // A taskset's semaphore (GTA V's), or one of the whole instance shared by the tasks of two tasksets (V finds a
    // waiter's taskset through its workload ID).
    const bool whole = GENERATE(false, true);
    CAPTURE(whole);
    constexpr u32 kSpus = 4, kUnits = 3, kIterations = 25;
    const u32 tasks = whole ? 12 : 10, waiters = tasks - kUnits;
    SemFixture s(true, kSpus, whole ? 2 : 1);
    u8* b = s.b;
    if (whole) {
        REQUIRE(s.a.call("_cellSpursSemaphoreInitialize", kSpurs, 0, kSem, kUnits) == 0);
    } else {
        REQUIRE(s.a.call("_cellSpursSemaphoreInitialize", 0, kTs, kSem, kUnits) == 0);
    }
    write_task_elf(b, kElf, semaphore_task());
    std::memset(b + kStats, 0, 0x100); // the stats and the controls: the gate closed, no pause
    std::vector<std::pair<u32, u32>> ids; // task i: (taskset, task ID)
    for (u32 i = 0; i < tasks; i++) {
        const u32 taskset = whole && (i & 1) ? kTs2 : kTs;
        std::memset(b + kReports + 32 * i, 0, 32);
        ids.emplace_back(taskset, s.create(taskset, kSem, kIterations, kReports + 32 * i, kCtx + kCtxSize * i));
    }

    // Three tasks take the units and stay in; every other task finds none, joins the waiters and switches out, its
    // context saved with the wait object semaphore | 1.
    const bool settled = wait_for([&] {
        const Line st = snapshot(b, kStats), sem = snapshot(b, kSem);
        if (st.word(0x00) != kUnits || sem.word(0x00) != 0 || sem.byte(0x10) != waiters || sem.byte(0x20) != waiters) {
            return false;
        }
        for (u32 k = 0; k < waiters; k++) {
            const u32 id = sem.byte(0x11 + k), wid = sem.byte(0x21 + k);
            const u32 taskset = wid == s.wid(kTs) ? kTs : kTs2;
            if (!s.task_bit(taskset, 0x50, id) || s.task_bit(taskset, 0x00, id) || s.task_bit(taskset, 0x10, id)) {
                return false; // not waiting yet, still running, or ready
            }
        }
        return true;
    }, 30000ms);
    if (!settled) dump_state(b);
    REQUIRE(settled);
    {
        const Line sem = snapshot(b, kSem);
        std::set<std::pair<u32, u32>> listed;
        for (u32 k = 0; k < waiters; k++) {
            const u32 id = sem.byte(0x11 + k), wid = sem.byte(0x21 + k);
            CAPTURE(k, id, wid);
            REQUIRE((wid == s.wid(kTs) || (whole && wid == s.wid(kTs2))));
            const u32 taskset = wid == s.wid(kTs) ? kTs : kTs2;
            listed.emplace(taskset, id);
            CHECK_FALSE(s.task_bit(taskset, 0x40, id)); // not signalled
            u32 i = 0;
            while (i < tasks && ids[i] != std::pair{taskset, id}) i++;
            REQUIRE(i < tasks);
            CHECK(ps_read64(b, kCtx + kCtxSize * i + 0x358) == (u64{kSem} | 1)); // LS 0x2FD8, saved
        }
        CHECK(listed.size() == waiters);
        CHECK(sem.byte(0x11 + waiters) == 0);
    }

    // The gate opens: from here every V hands its unit to the oldest waiter on whatever SPU, until all are done.
    set_control(b, 0, 1);
    const bool done = wait_for([&] { return snapshot(b, kStats).word(0x0C) == tasks; }, 60000ms);
    if (!done) dump_state(b);
    REQUIRE(done);
    const Line st = snapshot(b, kStats), sem = snapshot(b, kSem);
    CHECK(st.word(0x00) == 0);                    // nobody inside
    CHECK(st.word(0x04) == kUnits);               // never more inside than there are units
    CHECK(st.word(0x08) == tasks * kIterations);  // no entry lost
    CHECK(sem.word(0x00) == kUnits);              // every unit back in the count
    CHECK(sem.byte(0x10) == 0);
    CHECK(sem.byte(0x20) == 0);
    CHECK(ps_read64(b, kSem + 0x30) == (whole ? kSpurs : 0));
    CHECK(ps_read64(b, kSem + 0x38) == (whole ? 0 : kTs));
    u32 total_waits = 0;
    for (u32 i = 0; i < tasks; i++) {
        CAPTURE(i);
        CHECK(ps_read32(b, kReports + 32 * i) == kIterations);
        total_waits += ps_read32(b, kReports + 32 * i + 4);
    }
    CHECK(total_waits >= waiters);
    REQUIRE(wait_for([&] { return line_word(b, kTs + 0x30) == 0 && line_word(b, kTs2 + 0x30) == 0; })); // all exited
}

TEST_CASE("SPURS semaphores: a V from another taskset's task wakes a waiter through its workload's signal",
    "[hle][spurs][spurs-semaphore]")
{
    // A semaphore of the whole instance with one unit. A (taskset 2) holds it; B (taskset 1, which has nothing else to
    // run) waits. A's V signals B: B's signalled bit and, from SPU code, taskset 1's bit in wklSignal1, which alone
    // brings B's taskset back onto an SPU.
    SemFixture s(true, 2, 2);
    u8* b = s.b;
    REQUIRE(s.a.call("_cellSpursSemaphoreInitialize", kSpurs, 0, kSem, 1) == 0);
    write_task_elf(b, kElf, semaphore_task());
    std::memset(b + kStats, 0, 0x100);
    std::memset(b + kReports, 0, 64);
    s.create(kTs2, kSem, 1, kReports, kCtx);
    REQUIRE(wait_for([&] { return snapshot(b, kStats).word(0x00) == 1; }));
    const u32 tb = s.create(kTs, kSem, 1, kReports + 32, kCtx + kCtxSize);
    const u32 wid = s.wid(kTs);
    REQUIRE(wait_for([&] {
        return snapshot(b, kSem).byte(0x10) == 1 && s.task_bit(kTs, 0x50, tb) && !s.task_bit(kTs, 0x00, tb) &&
               line_byte(b, kSpurs + wid) == 0; // switched out, and its taskset's ready count is back to 0
    }));
    CHECK(snapshot(b, kSem).byte(0x11) == tb);
    CHECK(snapshot(b, kSem).byte(0x21) == wid);
    CHECK_FALSE(ps_read16(b, kSpurs + 0x70) & (0x8000u >> wid)); // no workload signal pending

    set_control(b, 0, 1);
    REQUIRE(wait_for([&] { return snapshot(b, kStats).word(0x0C) == 2; }));
    CHECK(ps_read32(b, kReports + 32) == 1);     // B's entry
    CHECK(ps_read32(b, kReports + 32 + 4) == 1); // after its one wait
    CHECK(snapshot(b, kSem).word(0x00) == 1);
    REQUIRE(wait_for([&] { return line_word(b, kTs + 0x30) == 0 && line_word(b, kTs2 + 0x30) == 0; }));
}

TEST_CASE("SPURS semaphores: a V's signal that reaches the waiter before its syscall 2 is taken at once",
    "[hle][spurs][spurs-semaphore]")
{
    SemFixture s(true, 2);
    u8* b = s.b;
    REQUIRE(s.a.call("_cellSpursSemaphoreInitialize", 0, kTs, kSem, 1) == 0);
    write_task_elf(b, kElf, semaphore_task());
    std::memset(b + kStats, 0, 0x100);
    ps_write32(b, kControl + 4, 1); // a P that has to wait stops before its syscall 2
    std::memset(b + kReports, 0, 64);

    // Task A takes the unit and stays in (the gate is closed).
    s.create(kTs, kSem, 1, kReports, kCtx);
    REQUIRE(wait_for([&] { return snapshot(b, kStats).word(0x00) == 1; }));
    // Task B finds no unit: it joins the waiters and stops there, still running on the other SPU.
    std::memset(b + kCtx + kCtxSize, 0xEE, kCtxSize);
    const u32 tb = s.create(kTs, kSem, 1, kReports + 32, kCtx + kCtxSize);
    REQUIRE(wait_for([&] { return load32_acquire(b, kReports + 32 + 16) == 1; })); // the pause marker's PUT
    {
        const Line sem = snapshot(b, kSem);
        CHECK(sem.word(0x00) == 0);
        CHECK(sem.byte(0x10) == 1);
        CHECK(sem.byte(0x11) == tb);
        CHECK(sem.byte(0x21) == s.wid(kTs));
    }
    CHECK(s.task_bit(kTs, 0x00, tb));       // running
    CHECK_FALSE(s.task_bit(kTs, 0x50, tb)); // not waiting
    CHECK_FALSE(s.task_bit(kTs, 0x40, tb)); // not signalled

    // A leaves: its V takes B off the list and signals it while it runs; the unit is B's, not the count's.
    set_control(b, 0, 1);
    REQUIRE(wait_for([&] { return snapshot(b, kStats).word(0x0C) == 1; }));
    {
        const Line sem = snapshot(b, kSem);
        CHECK(sem.word(0x00) == 0);
        CHECK(sem.byte(0x10) == 0);
        CHECK(sem.byte(0x20) == 0);
    }
    CHECK(s.task_bit(kTs, 0x40, tb)); // signalled
    CHECK(s.task_bit(kTs, 0x00, tb)); // still running
    CHECK_FALSE(s.task_bit(kTs, 0x50, tb));
    CHECK(ps_read32(b, kReports) == 1); // A's report: one entry, no wait
    CHECK(ps_read32(b, kReports + 4) == 0);

    // B goes on: its syscall 2 finds the signal and returns at once, without switching out (its context save area is
    // untouched), and it takes its turn.
    set_control(b, 8, 1);
    REQUIRE(wait_for([&] { return snapshot(b, kStats).word(0x0C) == 2; }));
    CHECK(ps_read32(b, kReports + 32) == 1);
    CHECK(ps_read32(b, kReports + 32 + 4) == 1);
    const Line st = snapshot(b, kStats), sem = snapshot(b, kSem);
    CHECK(st.word(0x04) == 1);
    CHECK(st.word(0x08) == 2);
    CHECK(sem.word(0x00) == 1);
    CHECK(sem.byte(0x10) == 0);
    bool untouched = true;
    for (u32 i = 0; i < kCtxSize; i++) untouched = untouched && b[kCtx + kCtxSize + i] == 0xEE;
    CHECK(untouched);
    REQUIRE(wait_for([&] { return line_word(b, kTs + 0x30) == 0; })); // both exited
}
