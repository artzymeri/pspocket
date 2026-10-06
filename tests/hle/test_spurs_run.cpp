// The native SPURS scheduler running synthetic SPU tasks (built in code with the SPU assembler: no game data):
// a task that exits, one that waits for a signal and resumes from its context save area, a PPU event flag set waking
// a waiting task, an SPU-to-PPU event through a SPURS port, event flag bits set from SPU code, two tasks yielding
// to each other on one SPU, two workloads taking one SPU in priority order, and a task whose translated program is
// that of the segment it is entered in.

#include "hle_test_util.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <functional>
#include <thread>
#include <vector>

#include "pspocket/core/reservation.h"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/spu/context.h"
#include "pspocket/spu/runtime.h"
#include "guest.hpp" // runtime/hle/src: reservation_op
#include "spurs.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::spu::as;

namespace {

constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kTsAttr = kData + 0x4000, kPrio = kData + 0x4400;
constexpr u32 kTs = kData + 0x6000; // 6400 bytes
constexpr u32 kId = kData + 0x8000, kArg = kData + 0x8100, kArg2 = kData + 0x8180, kPattern = kData + 0x8200;
constexpr u32 kFlag = kData + 0x8400, kQueueAttr = kData + 0x8500, kQueueId = kData + 0x8520, kPort = kData + 0x8530;
constexpr u32 kOut = kData + 0x9000, kOut2 = kData + 0x9100;
constexpr u32 kCtx = kData + 0xA000, kCtx2 = kData + 0xB000; // 0xC00 bytes each: 1 KB + one block
constexpr u32 kTs2 = kData + 0xC000, kTsAttr2 = kData + 0xE000, kCounter = kData + 0xE400; // a second taskset
constexpr u32 kElf = kData + 0x10000;
constexpr u32 kBuf = 0x3800; // the tasks' LS buffer for DMA

// An SPU task program, assembled at 0x3000.
struct Program {
    std::vector<u32> code;
    void emit(u32 insn) { code.push_back(insn); }
    void load32(u32 rt, u32 v)
    {
        emit(as::ilhu(rt, static_cast<s32>(v >> 16)));
        emit(as::iohl(rt, static_cast<s32>(v & 0xFFFF)));
    }
    // DMA 16 bytes between LS (address in `ls_reg`) and main memory (EA in `ea_reg`'s preferred word), then wait.
    void dma16(u32 cmd, u32 ls_reg, u32 ea_reg)
    {
        constexpr u32 t = 70, tag = 5;
        emit(as::wrch(PS_MFC_LSA, ls_reg));
        emit(as::il(t, 0));
        emit(as::wrch(PS_MFC_EAH, t));
        emit(as::wrch(PS_MFC_EAL, ea_reg));
        emit(as::il(t, 16));
        emit(as::wrch(PS_MFC_Size, t));
        emit(as::il(t, tag));
        emit(as::wrch(PS_MFC_TagID, t));
        emit(as::il(t, static_cast<s32>(cmd)));
        emit(as::wrch(PS_MFC_Cmd, t));
        emit(as::il(t, 1 << tag));
        emit(as::wrch(PS_MFC_WrTagMask, t));
        emit(as::il(t, PS_MFC_TAG_UPDATE_ALL));
        emit(as::wrch(PS_MFC_WrTagUpdate, t));
        emit(as::rdch(t, PS_MFC_RdTagStat));
    }
    // Stores register `value` at EA register `ea_reg` + `offset` (through the LS buffer).
    void put(u32 value, u32 ea_reg, s32 offset)
    {
        emit(as::il(71, kBuf));
        emit(as::stqd(value, 0, 71));
        emit(as::ai(72, ea_reg, offset));
        dma16(PS_MFC_PUT, 71, 72);
    }
    // Calls the taskset policy's syscall entry, as the SPU-side task library does.
    void syscall(u32 n)
    {
        emit(as::il(3, static_cast<s32>(n)));
        emit(as::ila(5, 0xA70));
        emit(as::bisl(0, 5));
    }
};

// A task ELF at `ea`: one read-only PT_LOAD at 0x3000 with the program (what libsre's loader accepts).
void write_task_elf(u8* base, u32 ea, const Program& p)
{
    const u32 size = static_cast<u32>(p.code.size() * 4);
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
    for (usize i = 0; i < p.code.size(); i++) ps_write32(base, ea + 0x100 + 4 * static_cast<u32>(i), p.code[i]);
}

bool wait_for(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// One SPURS instance with one SPU and one taskset (max contention 1) with priority `priority` on SPU 0 (0: the worker
// never selects it until the test raises it).
struct SpursFixture {
    Fixture f;
    GuestThread a{f, 0, "ppu"};
    u8* base = f.base();

    explicit SpursFixture(u8 priority = 1)
    {
        REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 1, 200, 2016, 0) == 0);
        REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
        const u8 prio[8] = {priority, 0, 0, 0, 0, 0, 0, 0};
        std::memcpy(base + kPrio, prio, 8);
        REQUIRE(a.call("_cellSpursTasksetAttributeInitialize", kTsAttr, 1, 0x330000, 0x1234, kPrio, 1) == 0);
        REQUIRE(a.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs, kTsAttr) == 0);
    }
    ~SpursFixture() { CHECK(a.call("cellSpursFinalize", kSpurs) == 0); }

    bool task_bit(u32 set_offset, u32 id) { return ps_read32(base, kTs + set_offset + 4 * (id / 32)) & (0x80000000u >> (id % 32)); }
    void set_pattern_top_block() // the stack's block (0x3F800..0x3FFFF) only
    {
        std::memset(base + kPattern, 0, 16);
        ps_write8(base, kPattern + 15, 1);
    }
};

// Waits for a signal, then reports what it resumed with: 1 at EA before, then $3 at EA+16 and $80 at EA+32.
Program waiting_task()
{
    Program p;
    p.emit(as::ila(1, 0x3FFD0)); // the SDK's crt0 stack
    p.load32(80, 0x80808080);    // callee-saved: must survive the switch
    p.emit(as::ori(81, 3, 0));   // the output EA (task argument word 0), kept in a callee-saved register
    p.emit(as::il(21, 1));
    p.put(21, 81, 0);
    p.syscall(2); // wait for a signal
    p.put(3, 81, 16);
    p.put(80, 81, 32);
    p.syscall(0);
    return p;
}

} // namespace

TEST_CASE("SPURS workers: a task starts with libsre's registers and local store, and exits", "[hle][spurs][spurs-run]")
{
    SpursFixture s;
    u8* b = s.base;
    Program p;
    p.emit(as::il(10, kBuf));
    p.emit(as::stqd(3, 0, 10));     // the 16-byte task argument
    p.dma16(PS_MFC_PUT, 10, 3);     // to the EA in its first word
    p.emit(as::stqd(4, 0, 10));     // $4: taskset arguments, SPURS address
    p.emit(as::ai(11, 3, 16));
    p.dma16(PS_MFC_PUT, 10, 11);
    p.emit(as::lqa(12, 0x27D0));    // the taskset context: DMA tag, task ID
    p.emit(as::stqd(12, 0, 10));
    p.emit(as::ai(11, 3, 32));
    p.dma16(PS_MFC_PUT, 10, 11);
    p.emit(as::lqa(12, 0x1E0));     // the kernel context: ..., module ID 'TK' at 0x1E8
    p.emit(as::stqd(12, 0, 10));
    p.emit(as::ai(11, 3, 48));
    p.dma16(PS_MFC_PUT, 10, 11);
    p.syscall(0);
    write_task_elf(b, kElf, p);
    ps_write32(b, kArg, kOut);
    ps_write32(b, kArg + 4, 0xCAFEBABE);
    ps_write32(b, kArg + 8, 0x11);
    ps_write32(b, kArg + 12, 0x22);

    REQUIRE(s.a.call("cellSpursCreateTask", kTs, kId, kElf, 0, 0, 0, kArg) == 0);
    const u32 id = ps_read32(b, kId);
    REQUIRE(wait_for([&] { return ps_read32(b, kOut + 48) != 0; }));
    CHECK(ps_read32(b, kOut + 0) == kOut);
    CHECK(ps_read32(b, kOut + 4) == 0xCAFEBABE);
    CHECK(ps_read32(b, kOut + 12) == 0x22);
    CHECK(ps_read64(b, kOut + 16) == 0x1234); // preferred doubleword: the taskset's arguments
    CHECK(ps_read64(b, kOut + 24) == kSpurs);
    CHECK(ps_read32(b, kOut + 32) == 31);     // DMA tag
    CHECK(ps_read32(b, kOut + 36) == id);     // task ID
    CHECK(ps_read32(b, kOut + 48) == 0x808);  // exit to kernel
    CHECK(ps_read16(b, kOut + 56) == 0x544B); // 'TK'
    // The exit destroys the task: no longer enabled, running or ready, and the ready count is back to 0.
    REQUIRE(wait_for([&] { return !s.task_bit(0x30, id); }));
    REQUIRE(wait_for([&] { return !s.task_bit(0x00, id) && ps_read8(b, kSpurs + 0x00) == 0; }));
    CHECK_FALSE(s.task_bit(0x10, id));
}

TEST_CASE("SPURS workers: a task waits for a signal in its context save area and resumes", "[hle][spurs][spurs-run]")
{
    SpursFixture s;
    u8* b = s.base;
    write_task_elf(b, kElf, waiting_task());
    ps_write32(b, kArg, kOut);
    s.set_pattern_top_block();
    REQUIRE(s.a.call("cellSpursCreateTask", kTs, kId, kElf, kCtx, 0xC00, kPattern, kArg) == 0);
    const u32 id = ps_read32(b, kId);

    // Phase 1 ran, then the task waits: saved to its context save area, the SPU free.
    REQUIRE(wait_for([&] { return ps_read32(b, kOut) == 1; }));
    REQUIRE(wait_for([&] { return s.task_bit(0x50, id) && !s.task_bit(0x00, id); }));
    CHECK_FALSE(s.task_bit(0x10, id));                // no longer ready
    CHECK(ps_read32(b, kCtx + 0x20) == 0x80808080);   // saved $80 (LS 0x2CA0)
    CHECK(ps_read32(b, kCtx + 0x10) == 0x3FFD0);      // saved $1
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(ps_read32(b, kOut + 16) == 0);              // still waiting

    REQUIRE(hle::spurs::send_signal(b, kTs, id) == lv2::CELL_OK);
    REQUIRE(wait_for([&] { return ps_read32(b, kOut + 32) == 0x80808080; }));
    CHECK(ps_read32(b, kOut + 16) == 0); // the syscall's result after a resume
    REQUIRE(wait_for([&] { return !s.task_bit(0x30, id); }));
    CHECK(hle::spurs::send_signal(b, kTs, id) == static_cast<s32>(0x80410905u)); // gone: SRCH
}

// PPU2SPU is how GTA V's DVD streamer hands work to its zlib task (regression: the PPU's set was refused with PERM,
// as RPCS3 checks the direction a PPU wait needs, and the streamer waited for the task's answer forever).
TEST_CASE("SPURS workers: cellSpursEventFlagSet from the PPU wakes a waiting task", "[hle][spurs][spurs-run]")
{
    const u32 direction = GENERATE(3u /* ANY2ANY */, 2u /* PPU2SPU */);
    CAPTURE(direction);
    SpursFixture s;
    u8* b = s.base;
    write_task_elf(b, kElf, waiting_task());
    ps_write32(b, kArg, kOut);
    s.set_pattern_top_block();
    REQUIRE(s.a.call("_cellSpursEventFlagInitialize", 0, kTs, kFlag, 0 /* auto clear */, direction) == 0);
    REQUIRE(s.a.call("cellSpursCreateTask", kTs, kId, kElf, kCtx, 0xC00, kPattern, kArg) == 0);
    const u32 id = ps_read32(b, kId);
    REQUIRE(wait_for([&] { return s.task_bit(0x50, id) && !s.task_bit(0x00, id); }));

    // What the SPU-side cellSpursEventFlagWait records before it waits: slot 0, mask 0x0003, OR, this task.
    ps_write16(b, kFlag + 0x08, 0x8000); // spuTaskUsedWaitSlots
    ps_write16(b, kFlag + 0x10, 0x0003); // spuTaskWaitMask[0]
    ps_write8(b, kFlag + 0x50, static_cast<u8>(id));
    REQUIRE(s.a.call("cellSpursEventFlagSet", kFlag, 0x0002) == 0);
    CHECK(ps_read16(b, kFlag + 0x30) == 0x0002); // pendingRecvTaskEvents[0]: what the task receives
    CHECK(ps_read16(b, kFlag + 0x02) == 0x8000); // spuTaskPendingRecv
    CHECK(ps_read16(b, kFlag + 0x00) == 0);      // auto clear
    REQUIRE(wait_for([&] { return ps_read32(b, kOut + 32) == 0x80808080; }));
    REQUIRE(wait_for([&] { return !s.task_bit(0x30, id); }));
}

TEST_CASE("SPURS workers: an SPU task throws an event to the PPU through a SPURS port", "[hle][spurs][spurs-run]")
{
    SpursFixture s;
    u8* b = s.base;
    ps_write32(b, kQueueAttr + 0, 2); // SYS_SYNC_PRIORITY
    ps_write32(b, kQueueAttr + 4, 1); // SYS_PPU_QUEUE
    REQUIRE(lv2::sys_event_queue_create(*s.a.t, kQueueId, kQueueAttr, 0, 16) == lv2::CELL_OK);
    const u32 queue = ps_read32(b, kQueueId);
    ps_write8(b, kPort, 5);
    REQUIRE(s.a.call("cellSpursAttachLv2EventQueue", kSpurs, queue, kPort, 0) == 0);

    // sys_spu_thread_throw_event(port 5, 0x123, 0x777), as the SPU-side SPURS library sends it.
    Program p;
    p.emit(as::il(20, 0x777));
    p.emit(as::wrch(PS_SPU_WrOutMbox, 20));
    p.load32(21, (0x45u << 24) | 0x123);
    p.emit(as::wrch(PS_SPU_WrOutIntrMbox, 21));
    p.syscall(0);
    write_task_elf(b, kElf, p);
    REQUIRE(s.a.call("cellSpursCreateTask", kTs, kId, kElf, 0, 0, 0, 0) == 0);

    const auto q = s.f.kernel->event_queue(queue);
    REQUIRE(q != nullptr);
    lv2::Event ev;
    REQUIRE(wait_for([&] { return q->try_receive(&ev, 1) == 1; }));
    CHECK(ev.source == 0xFFFFFFFF53505501ull); // SYS_SPU_THREAD_EVENT_USER_KEY
    CHECK(ev.data2 == ((u64{5} << 32) | 0x123));
    CHECK(ev.data3 == 0x777);
}

TEST_CASE("SPURS workers: SPU code sets lv2 event flag bits through its interrupt mailbox", "[hle][spurs][spurs-run]")
{
    SpursFixture s;
    u8* b = s.base;
    std::memset(b + kQueueAttr, 0, 32);      // sys_event_flag_attribute_t
    ps_write32(b, kQueueAttr + 0, 2);        // SYS_SYNC_PRIORITY
    ps_write32(b, kQueueAttr + 4, 0x200);    // SYS_SYNC_NOT_PROCESS_SHARED
    ps_write32(b, kQueueAttr + 20, 0x20000); // SYS_SYNC_WAITER_MULTIPLE
    REQUIRE(lv2::sys_event_flag_create(*s.a.t, kQueueId, kQueueAttr, 0) == lv2::CELL_OK);
    const u32 flag = ps_read32(b, kQueueId);

    // sys_event_flag_set_bit_impatient(flag, 5) (code 192, no answer), sys_event_flag_set_bit(flag, 40) (code 128)
    // and set_bit on a flag that doesn't exist, whose answers the task stores at its argument's EA.
    Program p;
    p.emit(as::ila(1, 0x3FFD0));
    p.emit(as::ori(81, 3, 0)); // the output EA (argument word 0)
    p.load32(20, flag);
    p.emit(as::wrch(PS_SPU_WrOutMbox, 20));
    p.load32(21, (192u << 24) | 5);
    p.emit(as::wrch(PS_SPU_WrOutIntrMbox, 21));
    p.emit(as::wrch(PS_SPU_WrOutMbox, 20));
    p.load32(21, (128u << 24) | 40);
    p.emit(as::wrch(PS_SPU_WrOutIntrMbox, 21));
    p.emit(as::rdch(22, PS_SPU_RdInMbox));
    p.put(22, 81, 0);
    p.load32(20, flag + 0x100);
    p.emit(as::wrch(PS_SPU_WrOutMbox, 20));
    p.emit(as::wrch(PS_SPU_WrOutIntrMbox, 21));
    p.emit(as::rdch(22, PS_SPU_RdInMbox));
    p.put(22, 81, 16);
    p.syscall(0);
    write_task_elf(b, kElf, p);
    ps_write32(b, kArg, kOut);
    ps_write32(b, kOut + 16, 0xFFFFFFFF);
    REQUIRE(s.a.call("cellSpursCreateTask", kTs, kId, kElf, 0, 0, 0, kArg) == 0);

    REQUIRE(wait_for([&] { return ps_read32(b, kOut + 16) != 0xFFFFFFFF; }));
    CHECK(s.f.kernel->event_flag(flag)->get() == ((u64{1} << 40) | (u64{1} << 5)));
    CHECK(ps_read32(b, kOut) == 0);                                                // CELL_OK
    CHECK(ps_read32(b, kOut + 16) == static_cast<u32>(lv2::err(lv2::CELL_ESRCH))); // no such flag
}

TEST_CASE("SPURS workers: two tasks yield to each other on one SPU", "[hle][spurs][spurs-run]")
{
    SpursFixture s(0); // both tasks exist before either runs: the taskset gets priority 1 on SPU 0 after that
    u8* b = s.base;
    // Writes 0xA0 | its number to its EA, yields, then copies the other task's EA (its first marker) to EA+16 and
    // exits. Each task can only see the other's marker if the yield let the other task run.
    Program p;
    p.emit(as::ila(1, 0x3FFD0));
    p.emit(as::ori(81, 3, 0));             // own EA (argument word 0)
    p.emit(as::rotqbyi(82, 3, 4));         // the other task's EA (argument word 1)
    p.emit(as::rotqbyi(83, 3, 8));         // marker (argument word 2)
    p.put(83, 81, 0);
    p.syscall(1); // yield
    p.emit(as::il(71, kBuf));
    p.dma16(PS_MFC_GET, 71, 82);           // the other task's marker
    p.emit(as::lqd(84, 0, 71));
    p.put(84, 81, 16);
    p.syscall(0);
    write_task_elf(b, kElf, p);
    s.set_pattern_top_block();
    ps_write32(b, kArg, kOut);
    ps_write32(b, kArg + 4, kOut2);
    ps_write32(b, kArg + 8, 0xA0);
    ps_write32(b, kArg2, kOut2);
    ps_write32(b, kArg2 + 4, kOut);
    ps_write32(b, kArg2 + 8, 0xA1);
    REQUIRE(s.a.call("cellSpursCreateTask", kTs, kId, kElf, kCtx, 0xC00, kPattern, kArg) == 0);
    REQUIRE(s.a.call("cellSpursCreateTask", kTs, kId + 4, kElf, kCtx2, 0xC00, kPattern, kArg2) == 0);
    ps_write8(b, kSpurs + 0xB00 + 0x18, 1); // WorkloadInfo.priority[0]
    REQUIRE(s.a.call("cellSpursWakeUp", kSpurs) == 0);
    const bool done = wait_for([&] { return ps_read32(b, kOut + 16) != 0 && ps_read32(b, kOut2 + 16) != 0; });
    if (!done) {
        for (u32 off = 0; off < 0x80; off += 0x10) {
            UNSCOPED_INFO(std::format("taskset +{:#04x}: {:08x} {:08x} {:08x} {:08x}", off, ps_read32(b, kTs + off),
                ps_read32(b, kTs + off + 4), ps_read32(b, kTs + off + 8), ps_read32(b, kTs + off + 12)));
        }
        for (u32 off = 0; off < 0x80; off += 0x10) {
            UNSCOPED_INFO(std::format("spurs +{:#04x}: {:08x} {:08x} {:08x} {:08x}", off, ps_read32(b, kSpurs + off),
                ps_read32(b, kSpurs + off + 4), ps_read32(b, kSpurs + off + 8), ps_read32(b, kSpurs + off + 12)));
        }
        UNSCOPED_INFO(std::format("out: {:x} {:x} / {:x} {:x}", ps_read32(b, kOut), ps_read32(b, kOut + 16),
            ps_read32(b, kOut2), ps_read32(b, kOut2 + 16)));
    }
    REQUIRE(done);
    CHECK(ps_read32(b, kOut) == 0xA0);
    CHECK(ps_read32(b, kOut2) == 0xA1);
    CHECK(ps_read32(b, kOut + 16) == 0xA1); // task 0 saw task 1's marker
    CHECK(ps_read32(b, kOut2 + 16) == 0xA0);
    REQUIRE(wait_for([&] { return ps_read32(b, kTs + 0x30) == 0; })); // both destroyed
}

// Regression (run 031): the kernel weighed the game's priority values themselves, so a workload at 12 won over one at 2
// (1 is the highest) and GTA V's AudioEngine job chain starved behind lower-priority workloads. libsre's system service
// gives the kernel 16 - priority, 0 staying "not on this SPU".
TEST_CASE("SPURS workers: of two ready workloads, the higher priority (the lower number) runs first",
    "[hle][spurs][spurs-run]")
{
    SpursFixture s(0); // workload 0, off SPU 0 until both tasks exist
    u8* b = s.base;
    REQUIRE(s.a.call("_cellSpursTasksetAttributeInitialize", kTsAttr2, 1, 0x330000, 0x5678, kPrio, 1) == 0);
    REQUIRE(s.a.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs2, kTsAttr2) == 0);
    REQUIRE(ps_read32(b, kTs2 + 0x74) == 1); // workload 1, also off SPU 0
    // Records its turn (the counter at argument word 1, then increments it) and the priorities its kernel context
    // holds. Neither task yields, so on one SPU the first runs to its exit before the second starts.
    Program p;
    p.emit(as::ila(1, 0x3FFD0));
    p.emit(as::ori(81, 3, 0));     // output EA (argument word 0)
    p.emit(as::rotqbyi(82, 3, 4)); // counter EA (argument word 1)
    p.emit(as::il(71, kBuf));
    p.dma16(PS_MFC_GET, 71, 82);
    p.emit(as::lqd(84, 0, 71));
    p.put(84, 81, 0);
    p.emit(as::ai(85, 84, 1));
    p.put(85, 82, 0);
    p.emit(as::lqa(86, 0x1A0)); // the kernel context's priorities on this SPU, one byte per workload
    p.put(86, 81, 16);
    p.syscall(0);
    write_task_elf(b, kElf, p);
    std::memset(b + kCounter, 0, 16);
    std::memset(b + kOut, 0xFF, 32);
    std::memset(b + kOut2, 0xFF, 32);
    ps_write32(b, kArg, kOut);
    ps_write32(b, kArg + 4, kCounter);
    ps_write32(b, kArg2, kOut2);
    ps_write32(b, kArg2 + 4, kCounter);
    REQUIRE(s.a.call("cellSpursCreateTask", kTs, kId, kElf, 0, 0, 0, kArg) == 0);
    REQUIRE(s.a.call("cellSpursCreateTask", kTs2, kId + 4, kElf, 0, 0, 0, kArg2) == 0);
    // Workload 1 gets priority 2 on SPU 0 (first, so a worker that wakes between the two stores picks it too), then
    // workload 0 priority 12.
    ps_write8(b, kSpurs + 0xB00 + 0x20 + 0x18, 2);
    ps_write8(b, kSpurs + 0xB00 + 0x18, 12);
    REQUIRE(s.a.call("cellSpursWakeUp", kSpurs) == 0);
    REQUIRE(wait_for([&] { return ps_read32(b, kOut) != 0xFFFFFFFF && ps_read32(b, kOut2) != 0xFFFFFFFF; }));
    CHECK(ps_read32(b, kOut2) == 0); // workload 1 (priority 2) ran first
    CHECK(ps_read32(b, kOut) == 1);
    CHECK(ps_read8(b, kOut2 + 16 + 1) == 14); // 16 - 2
    CHECK(ps_read8(b, kOut + 16) == 4);       // 16 - 12
    CHECK(ps_read8(b, kOut + 16 + 1) == 14);
    CHECK(ps_read8(b, kOut + 16 + 2) == 0); // no workload 2
    REQUIRE(wait_for([&] { return ps_read32(b, kTs + 0x30) == 0 && ps_read32(b, kTs2 + 0x30) == 0; }));
}

// Regression (found while bringing up the scheduler): the ready-count update kept its running total in a variable the
// atomic update changed, so an update retried after another writer touched CellSpurs line 0 lost the count, and ready
// tasks were never scheduled again. Here another thread keeps bumping the line's version while tasks are created and
// run, so updates retry all the time.
TEST_CASE("SPURS workers: ready counts stay right while other writers race for CellSpurs line 0",
    "[hle][spurs][spurs-run]")
{
    SpursFixture s;
    u8* b = s.base;
    Program p; // writes its argument word 1 to the EA in word 0, then exits
    p.emit(as::il(10, kBuf));
    p.emit(as::stqd(3, 0, 10));
    p.emit(as::rotqbyi(11, 3, 4));
    p.emit(as::stqd(11, 0, 10));
    p.dma16(PS_MFC_PUT, 10, 3);
    p.syscall(0);
    write_task_elf(b, kElf, p);

    std::atomic<bool> stop{false};
    std::thread racer([&] {
        while (!stop.load()) ps_res_notify_write(kSpurs, 128);
    });
    constexpr u32 kTasks = 8;
    for (u32 i = 0; i < kTasks; i++) {
        ps_write32(b, kArg + 16 * i, kOut + 16 * i);
        ps_write32(b, kArg + 16 * i + 4, 0x100 + i);
        REQUIRE(s.a.call("cellSpursCreateTask", kTs, kId, kElf, 0, 0, 0, kArg + 16 * i) == 0);
    }
    const bool done = wait_for([&] {
        for (u32 i = 0; i < kTasks; i++) {
            if (ps_read32(b, kOut + 16 * i) != 0x100 + i) return false;
        }
        return ps_read32(b, kTs + 0x30) == 0; // all destroyed
    });
    stop = true;
    racer.join();
    REQUIRE(done);
    REQUIRE(wait_for([&] { return ps_read8(b, kSpurs + 0x00) == 0; })); // ready count back to 0
}

// Regression (found with the SPURS semaphores, D-073): an idle worker's own selection stored CellSpurs line 0 even when
// it took nothing. Every store moves the line's version on, which ended the worker's wait for a change at once, so
// idle workers spun at full speed (four of them used 7.8 s of CPU in 2 s). Now the line stays as it is while nothing
// happens, and work still wakes the worker.
TEST_CASE("SPURS workers: an idle worker sleeps and leaves CellSpurs line 0 alone", "[hle][spurs][spurs-run]")
{
    SpursFixture s; // the taskset has no tasks: nothing to do
    u8* b = s.base;
    // The worker acknowledges the system service message that creating the taskset left (bit 0 of CellSpurs 0x72) the
    // next time it looks at the line, when its idle wait times out at the latest. After that nothing changes.
    REQUIRE(wait_for([&] { return !(ps_read8(b, kSpurs + 0x72) & 1); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const u64 version = ps_res_version(kSpurs);
    std::this_thread::sleep_for(std::chrono::milliseconds(350)); // the idle wait's 100 ms timeout passes three times
    CHECK(ps_res_version(kSpurs) == version);

    Program p; // writes its 16-byte argument to the EA in the argument's first word, then exits
    p.emit(as::il(10, kBuf));
    p.emit(as::stqd(3, 0, 10));
    p.dma16(PS_MFC_PUT, 10, 3);
    p.syscall(0);
    write_task_elf(b, kElf, p);
    ps_write32(b, kArg, kOut);
    ps_write32(b, kArg + 4, 0x5EE);
    REQUIRE(s.a.call("cellSpursCreateTask", kTs, kId, kElf, 0, 0, 0, kArg) == 0);
    REQUIRE(wait_for([&] { return ps_read32(b, kOut + 4) == 0x5EE; }));
}

TEST_CASE("SPURS workers: a game's own policy module runs with the kernel's entry points", "[hle][spurs][spurs-run]")
{
    constexpr u32 kPm = kData + 0x14000, kWid = kData + 0x8600, kFlagPtr = kData + 0x8610;
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 1, 200, 2016, 0) == 0);
    REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);

    // The module, entered at 0xA00 with $3 = 0x100, $4 = the argument (preferred doubleword), $5 = the poll status.
    // It runs again and again while it has work, so it records one run only: when the record's first word (the poll
    // status, never 0) is still 0. The test clears the record to have the next run recorded.
    Program p;
    p.emit(as::rotqbyi(80, 4, 4));    // the output EA: the argument's low word into the preferred slot
    p.emit(as::il(73, kBuf + 0x10));
    p.dma16(PS_MFC_GET, 73, 80);
    p.emit(as::lqd(74, 0, 73));
    const usize skip = p.code.size();
    p.emit(0);                         // brnz $74: recorded already
    p.put(5, 80, 0);                   // the poll status it was entered with
    p.emit(as::lqa(6, 0x1E0));
    p.emit(as::rotqbyi(6, 6, 4));      // [0x1E4]: the kernel's select-workload entry
    p.emit(as::il(3, 1));              // poll
    p.emit(as::bisl(0, 6));
    p.put(3, 80, 16);                  // {selected workload, poll status}
    p.emit(as::lqa(7, 0x1D0));
    p.put(7, 80, 32);                  // wklCurrentAddr, uniqueId, wklCurrentId
    p.code[skip] = as::brnz(74, static_cast<s32>(p.code.size() - skip) * 4);
    p.emit(as::lqa(2, 0x1E0));         // [0x1E0]: exit to the kernel
    p.emit(as::bisl(0, 2));
    for (usize i = 0; i < p.code.size(); i++) ps_write32(b, kPm + 4 * static_cast<u32>(i), p.code[i]);
    const u8 prio[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(b + kPrio, prio, 8);
    REQUIRE(a.call("cellSpursAddWorkload", kSpurs, kWid, kPm, static_cast<u32>(p.code.size() * 4), u64{kOut}, kPrio, 1,
                1) == 0);
    const u32 wid = ps_read32(b, kWid);

    // Work arrives as a ready count: the module runs (again and again while the count stays; the first run records).
    std::memset(b + kOut, 0, 48);
    REQUIRE(a.call("cellSpursReadyCountStore", kSpurs, wid, 1) == 0);
    REQUIRE(wait_for([&] { return ps_read64(b, kOut + 32) == kPm; })); // the module's last output
    REQUIRE(a.call("cellSpursReadyCountStore", kSpurs, wid, 0) == 0);
    CHECK(ps_read32(b, kOut + 0) == 1);  // entered with poll status 1: ready count
    CHECK(ps_read32(b, kOut + 16) == wid); // the poll keeps this workload
    CHECK(ps_read32(b, kOut + 20) == 1);   // ... with poll status 1
    CHECK(ps_read32(b, kOut + 44) == wid); // wklCurrentId
    CHECK(a.call("cellSpursReadyCountStore", kSpurs, wid, 256) == 0xFFFFFFFF80410802ull);
    CHECK(a.call("cellSpursReadyCountStore", kSpurs, wid + 1, 1) == 0xFFFFFFFF80410805ull); // not registered: SRCH

    // Work arrives as the workload flag: the receiver runs with poll status 4 and the flag is consumed.
    REQUIRE(wait_for([&] { return ps_read8(b, kSpurs + 0x20 + wid) == 0; })); // the last ready-count run is over
    REQUIRE(a.call("_cellSpursWorkloadFlagReceiver", kSpurs, wid, 1) == 0);
    CHECK(a.call("_cellSpursWorkloadFlagReceiver", kSpurs, wid, 1) == 0xFFFFFFFF8041080Aull); // already set: BUSY
    CHECK(a.call("_cellSpursWorkloadFlagReceiver", kSpurs, wid + 1, 1) == 0xFFFFFFFF80410805ull); // not registered
    REQUIRE(a.call("cellSpursGetWorkloadFlag", kSpurs, kFlagPtr) == 0);
    CHECK(ps_read32(b, kFlagPtr) == kSpurs + 0x60);
    std::memset(b + kOut, 0, 48);
    hle::reservation_op(b, kSpurs, [](u8* line) { // what code raising the flag does: store 0 into it
        ps_write32(line, 0x6C, 0);
        return true;
    });
    REQUIRE(wait_for([&] { return ps_read64(b, kOut + 32) == kPm; }));
    CHECK(ps_read32(b, kOut + 0) == 4); // poll status 4: the workload flag
    CHECK(ps_read32(b, kSpurs + 0x6C) == 0xFFFFFFFFu); // consumed by the selection
    REQUIRE(a.call("_cellSpursWorkloadFlagReceiver", kSpurs, wid, 0) == 0);
    CHECK(ps_read8(b, kSpurs + 0x77) == 0xFF);
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

// L.A. Noire (BLES00933) registers its own policy module with an attribute (D-165): _cellSpursWorkloadAttributeInitialize,
// cellSpursWorkloadAttributeSetName and cellSpursAddWorkloadWithAttribute, on an instance whose attribute asked for
// SPU printf and the system workload. The workload gets the attribute's module, data, priorities, names and shutdown
// hook, and runs as cellSpursAddWorkload's does.
TEST_CASE("SPURS workers: a policy module added with a workload attribute (L.A. Noire)", "[hle][spurs][spurs-run]")
{
    constexpr u32 kPm = kData + 0x14000, kWid = kData + 0x8600, kWattr = kData + 0x15000, kNames = kData + 0x15400,
                  kSwlPrio = kData + 0x15500, kPreempt = kData + 0x15510, kHandler = kData + 0x15600;
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    const u64 kCoreNull = r3_of_code(0x80410711), kCoreAlign = r3_of_code(0x80410710),
              kCoreInval = r3_of_code(0x80410702), kCorePerm = r3_of_code(0x80410709),
              kCoreBusy = r3_of_code(0x8041070A), kPmNull = r3_of_code(0x80410811), kPmAlign = r3_of_code(0x80410810),
              kPmInval = r3_of_code(0x80410802);

    // The SPURS attribute: SPU printf, then the system workload (RPCS3's checks).
    REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 2, 200, 2016, 0) == 0);
    CHECK(a.call("cellSpursAttributeEnableSpuPrintfIfAvailable", 0) == kCoreNull);
    CHECK(a.call("cellSpursAttributeEnableSpuPrintfIfAvailable", kSpursAttr + 4) == kCoreAlign);
    REQUIRE(a.call("cellSpursAttributeEnableSpuPrintfIfAvailable", kSpursAttr) == 0);
    CHECK(ps_read32(b, kSpursAttr + 0x28) == 0x10000000u);
    const u8 swl_prio[8] = {0, 1, 0, 0, 0, 0, 0, 0}, preempt[8] = {1, 1, 0, 0, 0, 0, 0, 0}, none[8] = {};
    std::memcpy(b + kPreempt, preempt, 8);
    std::memcpy(b + kSwlPrio, none, 8);
    CHECK(a.call("cellSpursAttributeEnableSystemWorkload", kSpursAttr, kSwlPrio, 1, kPreempt) == kCoreInval); // no 1
    std::memcpy(b + kSwlPrio, swl_prio, 8);
    CHECK(a.call("cellSpursAttributeEnableSystemWorkload", kSpursAttr, kSwlPrio, 0, kPreempt) == kCoreInval); // maxSpu
    REQUIRE(a.call("cellSpursAttributeEnableSystemWorkload", kSpursAttr, kSwlPrio, 1, kPreempt) == 0);
    CHECK(ps_read32(b, kSpursAttr + 0x28) == 0x12000000u);
    CHECK(std::memcmp(b + kSpursAttr + 0x38, swl_prio, 8) == 0);
    CHECK(ps_read32(b, kSpursAttr + 0x40) == 1);
    CHECK(ps_read32(b, kSpursAttr + 0x44) == 0b11);
    CHECK(a.call("cellSpursAttributeEnableSystemWorkload", kSpursAttr, kSwlPrio, 1, kPreempt) == kCoreBusy);
    {
        constexpr u32 kOne = kData + 0x15800;
        REQUIRE(a.call("_cellSpursAttributeInitialize", kOne, 2, 0x330000, 1, 200, 2016, 0) == 0);
        const u8 first[8] = {1, 0, 0, 0, 0, 0, 0, 0};
        std::memcpy(b + kSwlPrio + 8, first, 8);
        CHECK(a.call("cellSpursAttributeEnableSystemWorkload", kOne, kSwlPrio + 8, 1, kPreempt) == kCorePerm); // 1 SPU
    }
    // Initialized with the system workload: accepted (it does not run natively, as in RPCS3).
    REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);

    // The global exception handler: kept once (BUSY after), as RPCS3.
    CHECK(a.call("cellSpursSetGlobalExceptionEventHandler", kSpurs, 0, 0) == kCoreNull);
    REQUIRE(a.call("cellSpursSetGlobalExceptionEventHandler", kSpurs, kHandler, 0x1234) == 0);
    CHECK(ps_read64(b, kSpurs + 0xF80) == kHandler);
    CHECK(ps_read64(b, kSpurs + 0xF88) == 0x1234);
    CHECK(a.call("cellSpursSetGlobalExceptionEventHandler", kSpurs, kHandler, 0) == kCoreBusy);

    // The module: as in the cellSpursAddWorkload test above, it records its poll status and the kernel context once.
    Program p;
    p.emit(as::rotqbyi(80, 4, 4));
    p.emit(as::il(73, kBuf + 0x10));
    p.dma16(PS_MFC_GET, 73, 80);
    p.emit(as::lqd(74, 0, 73));
    const usize skip = p.code.size();
    p.emit(0);
    p.put(5, 80, 0);
    p.emit(as::lqa(7, 0x1D0));
    p.put(7, 80, 32);
    p.code[skip] = as::brnz(74, static_cast<s32>(p.code.size() - skip) * 4);
    p.emit(as::lqa(2, 0x1E0));
    p.emit(as::bisl(0, 2));
    for (usize i = 0; i < p.code.size(); i++) ps_write32(b, kPm + 4 * static_cast<u32>(i), p.code[i]);
    const u32 size = static_cast<u32>(p.code.size() * 4);
    const u8 prio[8] = {1, 1, 0, 0, 0, 0, 0, 0};
    std::memcpy(b + kPrio, prio, 8);

    // _cellSpursWorkloadAttributeInitialize's checks, then the attribute.
    CHECK(a.call("_cellSpursWorkloadAttributeInitialize", 0, 1, 0x330000, kPm, size, u64{kOut}, kPrio, 1, 1) == kPmNull);
    CHECK(a.call("_cellSpursWorkloadAttributeInitialize", kWattr + 4, 1, 0x330000, kPm, size, u64{kOut}, kPrio, 1, 1) ==
          kPmAlign);
    CHECK(a.call("_cellSpursWorkloadAttributeInitialize", kWattr, 1, 0x330000, 0, size, u64{kOut}, kPrio, 1, 1) == kPmNull);
    CHECK(a.call("_cellSpursWorkloadAttributeInitialize", kWattr, 1, 0x330000, kPm + 8, size, u64{kOut}, kPrio, 1, 1) ==
          kPmAlign);
    CHECK(a.call("_cellSpursWorkloadAttributeInitialize", kWattr, 1, 0x330000, kPm, size, u64{kOut}, kPrio, 0, 1) ==
          kPmInval); // no minimum contention
    ps_write8(b, kPrio + 7, 16);
    CHECK(a.call("_cellSpursWorkloadAttributeInitialize", kWattr, 1, 0x330000, kPm, size, u64{kOut}, kPrio, 1, 1) ==
          kPmInval); // a priority above 15
    ps_write8(b, kPrio + 7, 0);
    std::memset(b + kWattr, 0xAB, 0x200);
    REQUIRE(a.call("_cellSpursWorkloadAttributeInitialize", kWattr, 1, 0x330000, kPm, size, u64{kOut}, kPrio, 1, 2) == 0);
    CHECK(ps_read32(b, kWattr + 0x00) == 1);
    CHECK(ps_read32(b, kWattr + 0x08) == kPm);
    CHECK(ps_read32(b, kWattr + 0x0C) == size);
    CHECK(ps_read64(b, kWattr + 0x10) == kOut);
    CHECK(std::memcmp(b + kWattr + 0x18, prio, 8) == 0);
    CHECK(ps_read32(b, kWattr + 0x20) == 1);
    CHECK(ps_read32(b, kWattr + 0x24) == 2);
    CHECK(ps_read32(b, kWattr + 0x28) == 0); // names, hook: zeroed
    CHECK(ps_read32(b, kWattr + 0x1FC) == 0);
    std::memcpy(b + kNames, "LANoire\0Jobs", 13);
    CHECK(a.call("cellSpursWorkloadAttributeSetName", 0, kNames, kNames + 8) == kPmNull);
    REQUIRE(a.call("cellSpursWorkloadAttributeSetName", kWattr, kNames, kNames + 8) == 0);
    CHECK(ps_read32(b, kWattr + 0x28) == kNames);
    CHECK(ps_read32(b, kWattr + 0x2C) == kNames + 8);
    ps_write32(b, kWattr + 0x30, kHandler); // a shutdown-completion hook, as its setter would leave it
    ps_write32(b, kWattr + 0x34, 0x77);

    // cellSpursAddWorkloadWithAttribute: revision 1 only; then the workload, with the attribute's values.
    ps_write32(b, kWattr, 2);
    CHECK(a.call("cellSpursAddWorkloadWithAttribute", kSpurs, kWid, kWattr) == kPmInval);
    ps_write32(b, kWattr, 1);
    CHECK(a.call("cellSpursAddWorkloadWithAttribute", kSpurs, kWid, 0) == kPmNull);
    CHECK(a.call("cellSpursAddWorkloadWithAttribute", kSpurs, 0, kWattr) == kPmNull);
    REQUIRE(a.call("cellSpursAddWorkloadWithAttribute", kSpurs, kWid, kWattr) == 0);
    const u32 wid = ps_read32(b, kWid);
    REQUIRE(wid < 16);
    const u32 info = kSpurs + 0xB00 + 0x20 * wid;
    CHECK(ps_read64(b, info + 0x00) == kPm);
    CHECK(ps_read64(b, info + 0x08) == kOut);
    CHECK(ps_read32(b, info + 0x10) == size);
    CHECK(std::memcmp(b + info + 0x18, prio, 8) == 0);
    CHECK(ps_read64(b, kSpurs + 0xE00 + 0x10 * wid) == kNames);      // wklH1: nameClass
    CHECK(ps_read64(b, kSpurs + 0xE00 + 0x10 * wid + 8) == kNames + 8); // nameInstance
    CHECK(ps_read64(b, kSpurs + 0x100 + 0x80 * wid + 0x30) == kHandler); // wklF1: hook
    CHECK(ps_read64(b, kSpurs + 0x100 + 0x80 * wid + 0x38) == 0x77);
    CHECK((ps_read8(b, kSpurs + 0xA0 + wid) & 2) != 0); // wklEvent1: a hook is set
    CHECK(ps_read8(b, kSpurs + 0x50 + wid) % 16 == 2); // max contention

    // It runs when work arrives.
    std::memset(b + kOut, 0, 48);
    REQUIRE(a.call("cellSpursReadyCountStore", kSpurs, wid, 1) == 0);
    REQUIRE(wait_for([&] { return ps_read64(b, kOut + 32) == kPm; }));
    REQUIRE(a.call("cellSpursReadyCountStore", kSpurs, wid, 0) == 0);
    CHECK(ps_read32(b, kOut + 0) == 1);
    CHECK(ps_read32(b, kOut + 44) == wid);
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

namespace {

std::atomic<int> g_entry_segment_runs{0};

// The "translated" program of the task's second segment: counts its runs, then leaves the task to the interpreter.
void entry_segment_lifted(SPUContext* ctx, uint8_t*)
{
    g_entry_segment_runs.fetch_add(1);
    ps_spu_attach_program(ctx, nullptr);
}

} // namespace

TEST_CASE("SPURS workers: a task runs the translated program of the segment holding its entry (D-191)",
    "[hle][spurs][spurs-run]")
{
    // An SPU ELF with overlays starts with a small executable segment before the one it is entered in (the firmware's
    // encoders and video decoder): the task's program is the entry's segment, as for lv2's SPU threads. Before, the
    // first executable segment was hashed, no program matched it, and the whole task was interpreted.
    SpursFixture s;
    u8* b = s.base;
    Program p; // stores 0x77 at the EA in its argument's first word, then exits
    p.emit(as::il(10, kBuf));
    p.emit(as::il(12, 0x77));
    p.emit(as::stqd(12, 0, 10));
    p.dma16(PS_MFC_PUT, 10, 3);
    p.syscall(0);
    const u32 size = static_cast<u32>(p.code.size() * 4);
    std::memset(b + kElf, 0, 0x200 + size + 16);
    static constexpr u8 kIdent[16] = {0x7F, 'E', 'L', 'F', 1, 2, 1, 0};
    std::memcpy(b + kElf, kIdent, 16);
    ps_write16(b, kElf + 0x10, 2);    // ET_EXEC
    ps_write16(b, kElf + 0x12, 0x17); // EM_SPU
    ps_write32(b, kElf + 0x14, 1);
    ps_write32(b, kElf + 0x18, 0x3100); // entry: the second segment
    ps_write32(b, kElf + 0x1C, 0x34);
    ps_write16(b, kElf + 0x28, 0x34);
    ps_write16(b, kElf + 0x2A, 0x20);
    ps_write16(b, kElf + 0x2C, 2);
    const auto phdr = [&](u32 i, u32 offset, u32 vaddr, u32 filesz) {
        const u32 ph = kElf + 0x34 + 0x20 * i;
        ps_write32(b, ph + 0x00, 1); // PT_LOAD
        ps_write32(b, ph + 0x04, offset);
        ps_write32(b, ph + 0x08, vaddr);
        ps_write32(b, ph + 0x0C, vaddr);
        ps_write32(b, ph + 0x10, filesz);
        ps_write32(b, ph + 0x14, (filesz + 15) & ~15u);
        ps_write32(b, ph + 0x18, 5); // R X
        ps_write32(b, ph + 0x1C, 0x80);
    };
    phdr(0, 0x100, 0x3000, 0x10);
    phdr(1, 0x200, 0x3100, size);
    for (u32 i = 0; i < 4; i++) ps_write32(b, kElf + 0x100 + 4 * i, 0x00000007); // stop 7: never run
    for (usize i = 0; i < p.code.size(); i++) ps_write32(b, kElf + 0x200 + 4 * static_cast<u32>(i), p.code[i]);
    ps_write32(b, kArg, kOut);

    const ps_spu_entry entries[] = {{0x3100, &entry_segment_lifted}};
    ps_spu_program program{"spu_entry_segment", ps_spu_hash64(b + kElf + 0x200, size), 0x3100, size,
        PS_SPU_XFLOAT_APPROXIMATE, 0, 1, entries, nullptr, nullptr, 0};
    ps_spu_register_program(&program);
    g_entry_segment_runs = 0;
    REQUIRE(s.a.call("cellSpursCreateTask", kTs, kId, kElf, 0, 0, 0, kArg) == 0);
    REQUIRE(wait_for([&] { return ps_read32(b, kOut) == 0x77; }));
    CHECK(g_entry_segment_runs.load() == 1);
    ps_spu_unregister_program(&program);
}

// ------------------------------------------------------------------------------------------------- exit callbacks

namespace {

constexpr u32 kTs2Attr = kData + 0x2C000, kTs2B = kData + 0x30000; // a CellSpursTaskset2 (10496 bytes)
constexpr u32 kCallback = kData + 0x34000;                          // an exit callback's code (128-aligned)
constexpr u32 kContainer = kData + 0x35000;                         // an exit code container (128-aligned)
constexpr u32 kOut3 = kData + 0x36000;

// A CellSpursTaskset2 that no SPU selects until `run` gives it priority 1 on SPU 0, so its tasks can be prepared first.
struct Taskset2Fixture : SpursFixture {
    u32 wid = 0;
    Taskset2Fixture() : SpursFixture(0)
    {
        REQUIRE(a.call("_cellSpursTasksetAttribute2Initialize", kTs2Attr, 1) == 0);
        std::memset(base + kTs2Attr + 0x10, 0, 8); // priorities: on no SPU yet
        REQUIRE(a.call("cellSpursCreateTaskset2", kSpurs, kTs2B, kTs2Attr) == 0);
        wid = ps_read32(base, kTs2B + 0x74);
    }
    void run()
    {
        REQUIRE(a.call("cellSpursSetPriority", kSpurs, wid, 0, 1) == 0);
        REQUIRE(a.call("cellSpursWakeUp", kSpurs) == 0);
    }
    // Marks task `id` as having an exit callback (ELF bit 0) with `descriptor` (EA | size in 128-byte units) and
    // `argument`, as Sony's libvdec's tasks and libsre's cellSpursCreateTaskWithAttribute leave it.
    void set_exit_callback(u32 id, u64 descriptor, u64 argument)
    {
        const u32 info = kTs2B + 0x80 + 0x30 * id;
        ps_write64(base, info + 0x10, ps_read64(base, info + 0x10) | 1);
        ps_write64(base, kTs2B + 0x1980 + 16 * id, descriptor);
        ps_write64(base, kTs2B + 0x1988 + 16 * id, argument);
    }
};

// A task that writes 1 to the EA in its argument, then exits with `code` (cellSpursTaskExit puts it at 0x2FD0).
Program exiting_task(u32 code)
{
    Program p;
    p.emit(as::ila(1, 0x3FFD0));
    p.emit(as::ori(81, 3, 0));
    p.emit(as::il(21, 1));
    p.put(21, 81, 0);
    p.load32(20, code);
    p.emit(as::ila(22, 0x2FD0));
    p.emit(as::stqd(20, 0, 22));
    p.syscall(0);
    return p;
}

} // namespace

TEST_CASE("SPURS workers: an exiting task's exit callback runs with the taskset, task ID, exit code and argument (D-195)",
    "[hle][spurs][spurs-run]")
{
    Taskset2Fixture s;
    u8* b = s.base;
    write_task_elf(b, kElf, exiting_task(0x55));
    ps_write32(b, kArg, kOut3);
    // The callback (loaded at 0x10000): $5 (exit code) to the EA in $6 (the argument, a doubleword: its low word is
    // bytes 4..7, as libsre's handler reads it), $4 (task ID) after it, then $3 (the taskset), and returns.
    Program cb;
    cb.emit(as::rotqbyi(81, 6, 4));
    cb.put(5, 81, 0x40);
    cb.put(4, 81, 0x50);
    cb.put(3, 81, 0x60);
    cb.emit(as::bi(0));
    std::memset(b + kCallback, 0, 0x400);
    for (usize i = 0; i < cb.code.size(); i++) ps_write32(b, kCallback + 4 * static_cast<u32>(i), cb.code[i]);
    const u32 units = static_cast<u32>((cb.code.size() * 4 + 127) / 128);

    REQUIRE(s.a.call("cellSpursCreateTask", kTs2B, kId, kElf, 0, 0, 0, kArg) == 0);
    const u32 id = ps_read32(b, kId);
    s.set_exit_callback(id, kCallback | units, kOut3);
    s.run();
    REQUIRE(wait_for([&] { return ps_read32(b, kOut3 + 0x40) != 0; }));
    CHECK(ps_read32(b, kOut3) == 1);              // the task ran
    CHECK(ps_read32(b, kOut3 + 0x40) == 0x55);    // its exit code
    CHECK(ps_read32(b, kOut3 + 0x50) == id);      // its ID
    CHECK(ps_read64(b, kOut3 + 0x60) == kTs2B);   // the taskset (the preferred doubleword)
    REQUIRE(wait_for([&] { return ps_read32(b, kTs2B + 0x30) == 0; })); // destroyed
}

TEST_CASE("SPURS workers: libsre's exit code handler stores the code in the container and signals the waiting task "
          "(D-195)",
    "[hle][spurs][spurs-run]")
{
    Taskset2Fixture s;
    u8* b = s.base;
    // Task 0 waits for a signal (and then reports, as waiting_task does); task 1 exits with 0x77 into the container,
    // whose status says it is initialized (bit 0) and that task 0 of this taskset waits (bit 17).
    write_task_elf(b, kElf, waiting_task());
    write_task_elf(b, kElf + 0x1000, exiting_task(0x77));
    s.set_pattern_top_block();
    ps_write32(b, kArg, kOut);
    ps_write32(b, kArg2, kOut2);
    REQUIRE(s.a.call("cellSpursCreateTask", kTs2B, kId, kElf, kCtx, 0xC00, kPattern, kArg) == 0);
    REQUIRE(s.a.call("cellSpursCreateTask", kTs2B, kId + 4, kElf + 0x1000, 0, 0, 0, kArg2) == 0);
    const u32 waiter = ps_read32(b, kId), exiter = ps_read32(b, kId + 4);
    std::memset(b + kContainer, 0, 0x80);
    ps_write32(b, kContainer + 0x20, 1u | (1u << 17));
    ps_write64(b, kContainer + 0x30, kTs2B);
    ps_write8(b, kContainer + 0x38, static_cast<u8>(waiter));
    s.set_exit_callback(exiter, ps_read64(b, kTs2B + 0x1900), kContainer); // libsre's handler (its descriptor at 0x1900)
    s.run();

    REQUIRE(wait_for([&] { return ps_read32(b, kOut2) == 1; }));
    REQUIRE(wait_for([&] { return ps_read32(b, kOut + 32) == 0x80808080; })); // the waiter was signalled and resumed
    CHECK(ps_read32(b, kContainer + 0x20) == (1u | 4u | (1u << 17)));        // an exit code stored
    CHECK(ps_read32(b, kContainer + 0x28) == 0x77);
    REQUIRE(wait_for([&] { return ps_read32(b, kTs2B + 0x30) == 0; }));       // both destroyed
}
