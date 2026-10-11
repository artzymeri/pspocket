// cellSpurs' last functions on the native SPURS, made without libsre to compare with (unverified, D-594), with
// synthetic SPU tasks (assembled here: no game data): CellSpursTaskset2's own tasks, created, run and joined with their
// exit codes (D-626); task exit code containers (D-627); the LF queue's bodies and lv2 event queue through SPURS, with
// PPU threads on one side and SPU code (the host on its behalf, or a waiting task) on the other (D-628); the job
// pipeline's answer and the system workload given up (D-629).

#include "hle_test_util.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string_view>
#include <functional>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include "pspocket/core/reservation.h"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/spu/context.h"
#include "guest.hpp" // runtime/hle/src: reservation_op
#include "spurs.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::spu::as;

namespace {

constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kTsAttr = kData + 0x4000, kTs2Attr = kData + 0x4400,
              kPrio = kData + 0x4800;
constexpr u32 kTs2 = kData + 0x6000;   // a CellSpursTaskset2 (10496 bytes), from cellSpursCreateTaskset2
constexpr u32 kTs1 = kData + 0x9000;   // a CellSpursTaskset (6400 bytes)
constexpr u32 kTsBig = kData + 0xB000; // a CellSpursTaskset2 made through the attribute's size: no event flags
constexpr u32 kNames = kData + 0xE000; // the task name buffer (4 KB)
constexpr u32 kId = kData + 0xF000, kArg = kData + 0xF100, kAttr2 = kData + 0xF400, kBinInfo = kData + 0xF600,
              kPattern = kData + 0xF700, kExit = kData + 0xF800, kContainer = kData + 0xF880, kTaskAttr = kData + 0xFA00;
constexpr u32 kOut = kData + 0x10000;  // each task's reports, 0x100 apart
constexpr u32 kCtx = kData + 0x20000;  // context save areas: 1 KB + one LS block, 0x1000 apart
constexpr u32 kElf = kData + 0x30000;  // task ELFs, 0x1000 apart
constexpr u32 kLfq = kData + 0x40000, kLfqBuf = kData + 0x41000, kSrc = kData + 0x42000, kDst = kData + 0x42100,
              kOutWord = kData + 0x42200, kLfq2 = kData + 0x42400;
constexpr u32 kChain = kData + 0x50000, kCmds = kData + 0x50200;
constexpr u32 kUnmapped = 0x0FFF0000;
constexpr u32 kBuf = 0x3800; // the tasks' LS buffer for DMA

constexpr u32 TASK = 0x80410900, CORE = 0x80410700, JOB = 0x80410A00, POLICY = 0x80410800;
constexpr u32 kAgain = 0x01, kInval = 0x02, kSrch = 0x05, kNoexec = 0x07, kPerm = 0x09, kBusy = 0x0A, kFault = 0x0D,
              kStat = 0x0F, kAlign = 0x10, kNull = 0x11, kShutdown = 0x20;
u64 code(u32 c) { return r3_of_code(c); }

bool wait_for(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// An SPU task program, assembled at 0x3000 (test_spurs_run.cpp's).
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
    // cellSpursTaskExit(code): the code stored as a word at 0x2FD0 (the quadword's other words kept), then the exit
    // syscall.
    void exit_with(u32 code_value)
    {
        load32(20, code_value);
        exit_with_register(20);
    }
    void exit_with_register(u32 value)
    {
        emit(as::ila(22, 0x2FD0));
        emit(as::lqd(23, 0, 22));
        emit(as::cwd(24, 22, 0));
        emit(as::shufb(23, value, 23, 24));
        emit(as::stqd(23, 0, 22));
        syscall(0);
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

// Writes 1 to the EA in its argument's first word, then exits with `code_value`.
Program exiting_task(u32 code_value)
{
    Program p;
    p.emit(as::ila(1, 0x3FFD0)); // the SDK's crt0 stack
    p.emit(as::ori(81, 3, 0));   // the output EA (task argument word 0), in a callee-saved register
    p.emit(as::il(21, 1));
    p.put(21, 81, 0);
    p.exit_with(code_value);
    return p;
}

// Writes 1 to the EA in its argument, waits for a signal, writes 2 at EA + 16, then exits with `code_value`.
Program waiting_task(u32 code_value)
{
    Program p;
    p.emit(as::ila(1, 0x3FFD0));
    p.emit(as::ori(81, 3, 0));
    p.emit(as::il(21, 1));
    p.put(21, 81, 0);
    p.syscall(2); // wait for a signal
    p.emit(as::il(21, 2));
    p.put(21, 81, 16);
    p.exit_with(code_value);
    return p;
}

// The pattern a waiting task needs: the stack's block (0x3F800..0x3FFFF) only.
void top_block_pattern(u8* b, u32 at)
{
    std::memset(b + at, 0, 16);
    ps_write8(b, at + 15, 1);
}

hle::ProcessConfig config(bool workers)
{
    hle::ProcessConfig c;
    c.spurs_workers = workers;
    return c;
}

// One SPURS instance with one SPU and a CellSpursTaskset2 (cellSpursCreateTaskset2, with a task name buffer) that no SPU
// selects until run() gives it priority 1 on SPU 0, so tasks can be prepared first.
struct Task2Fixture {
    Fixture f;
    GuestThread a{f, 0, "ppu"};
    u8* b = f.base();
    u32 wid = 0;

    explicit Task2Fixture(bool workers = true) : f(config(workers))
    {
        REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 1, 200, 2016, 0) == 0);
        REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
        REQUIRE(a.call("_cellSpursTasksetAttribute2Initialize", kTs2Attr, 1) == 0);
        std::memset(b + kTs2Attr + 0x10, 0, 8); // priorities: on no SPU yet
        ps_write32(b, kTs2Attr + 0x20, kNames);
        REQUIRE(a.call("cellSpursCreateTaskset2", kSpurs, kTs2, kTs2Attr) == 0);
        wid = ps_read32(b, kTs2 + 0x74);
    }
    ~Task2Fixture() { CHECK(a.call("cellSpursFinalize", kSpurs) == 0); }

    void run()
    {
        REQUIRE(a.call("cellSpursSetPriority", kSpurs, wid, 0, 1) == 0);
        REQUIRE(a.call("cellSpursWakeUp", kSpurs) == 0);
    }
    // A task's bit in one of the taskset's bit sets, through the reservation system (D-585).
    bool task_bit(u32 set_offset, u32 id) const
    {
        return line_word(b, kTs2 + set_offset + 4 * (id / 32)) & (0x80000000u >> (id % 32));
    }
    // cellSpursCreateTask2 of `p` (ELF slot `n`, output kOut + 0x100 * n), with an attribute naming it and, when
    // `context`, giving it a context save area for the stack's block; the task's ID.
    u32 create(u32 n, const Program& p, const char* name, bool context = false)
    {
        write_task_elf(b, kElf + 0x1000 * n, p);
        std::memset(b + kArg, 0, 16);
        ps_write32(b, kArg, kOut + 0x100 * n);
        REQUIRE(a.call("_cellSpursTaskAttribute2Initialize", kAttr2, 1) == 0);
        if (context) {
            ps_write32(b, kAttr2 + 0x04, 0xC00);
            ps_write64(b, kAttr2 + 0x08, kCtx + 0x1000 * n);
            top_block_pattern(b, kAttr2 + 0x10);
        }
        std::strcpy(reinterpret_cast<char*>(b + kAttr2 + 0x80), name);
        ps_write32(b, kAttr2 + 0x20, name[0] ? kAttr2 + 0x80 : 0);
        REQUIRE(a.call("cellSpursCreateTask2", kTs2, kId, kElf + 0x1000 * n, kArg, kAttr2) == 0);
        return ps_read32(b, kId);
    }
    u32 report(u32 n, u32 off = 0) const { return load32_acquire(b, kOut + 0x100 * n + off); }
};

// A thread calling one HLE function that may block: `result` stays ~0 until it returns.
class Caller {
public:
    Caller(Fixture& f, u32 index, std::string_view fn, std::initializer_list<u64> args)
        : fn_(GuestThread::find(fn)), args_(args)
    {
        thread_ = std::thread([this, &f, index] {
            GuestThread g(f, index, "caller");
            lv2_thread = g.t;
            try {
                u32 r = 3;
                for (const u64 v : args_) g.ctx.gpr[r++] = v;
                fn_.fn(g.ctx, g.f.base());
                result = g.ctx.gpr[3];
            } catch (const lv2::ThreadExit&) {
                unwound = true;
            }
            lv2_thread = nullptr;
        });
    }
    ~Caller() { join(); }
    Caller(const Caller&) = delete;
    Caller& operator=(const Caller&) = delete;

    bool done() const { return result.load() != ~0ull; }
    void join()
    {
        if (thread_.joinable()) thread_.join();
    }
    bool blocked()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return !done() && !unwound;
    }
    // Parked on an lv2 object of this kind (the taskset's event flag a JoinTask2 waits on).
    bool parked_on(lv2::WaitKind kind)
    {
        return wait_for([&] {
            const lv2::PpuThread* t = lv2_thread.load();
            return t && t->waiter.waiting_on().kind == kind;
        });
    }

    std::atomic<u64> result{~0ull};
    std::atomic<bool> unwound{false};
    std::atomic<lv2::PpuThread*> lv2_thread{nullptr};

private:
    const hle::Function& fn_;
    const std::vector<u64> args_;
    std::thread thread_;
};

} // namespace

TEST_CASE("cellSpurs: the functions made without libsre to compare with, under their NIDs and unverified (D-594)",
    "[hle][spurs][spurs-task2]")
{
    for (const char* name : {"cellSpursCreateTask2", "cellSpursCreateTask2WithBinInfo", "cellSpursJoinTask2",
             "cellSpursTryJoinTask2", "cellSpursDestroyTaskset2", "cellSpursLFQueueAttachLv2EventQueue",
             "cellSpursLFQueueDetachLv2EventQueue"}) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("cellSpurs", recomp::nid_of(name));
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
        CHECK(fn->unverified);
        CHECK_FALSE(fn->internal);
    }
    // The functions they build on stay verified, and those read from libsre since (D-914).
    for (const char* name : {"cellSpursCreateTaskset2", "cellSpursCreateTaskWithAttribute", "cellSpursTaskExitCodeInitialize",
             "_cellSpursLFQueueInitialize", "cellSpursTaskExitCodeGet", "cellSpursTaskExitCodeTryGet",
             "_cellSpursLFQueuePushBody", "_cellSpursLFQueuePopBody", "cellSpursGetJobPipelineInfo",
             "cellSpursRemoveSystemWorkloadForUtility"}) {
        INFO(name);
        CHECK_FALSE(hle::registry().is_unverified("cellSpurs", recomp::nid_of(name)));
    }
}

// ------------------------------------------------------------------------------------------------- D-626

TEST_CASE("cellSpursCreateTask2: its checks; a CellSpursTaskset2's tasks run, exit and are joined with their exit "
          "codes (D-626)", "[hle][spurs][spurs-task2]")
{
    Task2Fixture s;
    u8* b = s.b;
    write_task_elf(b, kElf, exiting_task(0x11));
    std::memset(b + kPrio, 0, 8);
    REQUIRE(s.a.call("cellSpursCreateTaskset", kSpurs, kTs1, 0, kPrio, 1) == 0);
    REQUIRE(s.a.call("_cellSpursTasksetAttributeInitialize", kTsAttr, 1, 0x330000, 0, kPrio, 1) == 0);
    REQUIRE(s.a.call("cellSpursTasksetAttributeSetTasksetSize", kTsAttr, 10496) == 0);
    REQUIRE(s.a.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTsBig, kTsAttr) == 0);

    // The taskset's checks: NULL, ALIGN, memory; a CellSpursTaskset PERM; a CellSpursTaskset2 without the event flags
    // cellSpursCreateTaskset2 makes (one made through the attribute's size) STAT.
    CHECK(s.a.call("cellSpursCreateTask2", kTs2, 0, kElf, 0, 0) == code(TASK | kNull));
    CHECK(s.a.call("cellSpursCreateTask2", 0, kId, kElf, 0, 0) == code(TASK | kNull));
    CHECK(s.a.call("cellSpursCreateTask2", kTs2 + 0x40, kId, kElf, 0, 0) == code(TASK | kAlign));
    CHECK(s.a.call("cellSpursCreateTask2", kUnmapped, kId, kElf, 0, 0) == code(TASK | kFault));
    CHECK(s.a.call("cellSpursCreateTask2", kTs1, kId, kElf, 0, 0) == code(TASK | kPerm));
    CHECK(s.a.call("cellSpursCreateTask2", kTsBig, kId, kElf, 0, 0) == code(TASK | kStat));
    // The task's own (create_task's): the ELF NULL, misaligned or not an SPU task's, a context too small, the attribute
    // or the name not guest memory.
    CHECK(s.a.call("cellSpursCreateTask2", kTs2, kId, 0, 0, 0) == code(TASK | kNull));
    CHECK(s.a.call("cellSpursCreateTask2", kTs2, kId, kElf + 8, 0, 0) == code(TASK | kAlign));
    std::memset(b + kElf + 0x9000, 0, 0x100);
    CHECK(s.a.call("cellSpursCreateTask2", kTs2, kId, kElf + 0x9000, 0, 0) == code(TASK | kNoexec));
    CHECK(s.a.call("cellSpursCreateTask2", kTs2, kId, kElf, 0, kUnmapped) == code(TASK | kFault));
    REQUIRE(s.a.call("_cellSpursTaskAttribute2Initialize", kAttr2, 1) == 0);
    ps_write32(b, kAttr2 + 0x04, 0x200);
    ps_write64(b, kAttr2 + 0x08, kCtx);
    CHECK(s.a.call("cellSpursCreateTask2", kTs2, kId, kElf, 0, kAttr2) == code(TASK | kInval));
    REQUIRE(s.a.call("_cellSpursTaskAttribute2Initialize", kAttr2, 1) == 0);
    ps_write32(b, kAttr2 + 0x20, kUnmapped);
    CHECK(s.a.call("cellSpursCreateTask2", kTs2, kId, kElf, 0, kAttr2) == code(TASK | kFault));
    CHECK(ps_read32(b, kTs2 + 0x30) == 0); // a refused call takes no ID

    // Three tasks: the ELF word's bit 2 marks them; their names in the task name buffer, cut to 31 characters.
    const std::string long_name = "a name of more than thirty-one characters";
    const u32 t0 = s.create(0, exiting_task(0x11), "first");
    const u32 t1 = s.create(1, exiting_task(0x22), long_name.c_str());
    const u32 t2 = s.create(2, exiting_task(0x33), "");
    CHECK(t0 == 0);
    CHECK(t1 == 1);
    CHECK(t2 == 2);
    CHECK(ps_read64(b, kTs2 + 0x80 + 0x30 * t1 + 0x10) == ((kElf + 0x1000) | 4));
    CHECK(std::string(reinterpret_cast<const char*>(b + kNames + 32 * t0)) == "first");
    CHECK(std::string(reinterpret_cast<const char*>(b + kNames + 32 * t1)) == long_name.substr(0, 31));
    CHECK(b[kNames + 32 * t2] == 0);

    // Not run yet: TryJoinTask2 is BUSY. JoinTask2's checks.
    CHECK(s.a.call("cellSpursTryJoinTask2", kTs2, t0, kExit) == code(TASK | kBusy));
    CHECK(s.a.call("cellSpursJoinTask2", 0, t0, kExit) == code(TASK | kNull));
    CHECK(s.a.call("cellSpursJoinTask2", kTs2 + 0x40, t0, kExit) == code(TASK | kAlign));
    CHECK(s.a.call("cellSpursJoinTask2", kTs1, t0, kExit) == code(TASK | kPerm));
    CHECK(s.a.call("cellSpursJoinTask2", kTs2, 128, kExit) == code(TASK | kInval));
    CHECK(s.a.call("cellSpursJoinTask2", kTs2, 5, kExit) == code(TASK | kSrch)); // no such task
    CHECK(s.a.call("cellSpursJoinTask2", kTs2, t0, kUnmapped) == code(TASK | kFault));
    CHECK(s.a.call("cellSpursTryJoinTask2", kTs2, 200, kExit) == code(TASK | kInval));

    // They run; joined in another order than they ran, each with its own code from its exit slot.
    s.run();
    REQUIRE(s.a.call("cellSpursJoinTask2", kTs2, t2, kExit) == 0);
    CHECK(ps_read32(b, kExit) == 0x33);
    CHECK(s.report(2) == 1);
    REQUIRE(s.a.call("cellSpursJoinTask2", kTs2, t0, kExit) == 0);
    CHECK(ps_read32(b, kExit) == 0x11);
    REQUIRE(wait_for([&] { return s.a.call("cellSpursTryJoinTask2", kTs2, t1, kExit) == 0; }));
    CHECK(ps_read32(b, kExit) == 0x22);
    CHECK(ps_read32(b, kTs2 + 0x1980 + 16 * t1) == 0x22);
    CHECK(ps_read64(b, kTs2 + 0x1988 + 16 * t1) == 0); // no exit code container
    CHECK(s.a.call("cellSpursJoinTask2", kTs2, t1, kExit) == code(TASK | kSrch)); // joined already
    CHECK(s.a.call("cellSpursTryJoinTask2", kTs2, t0, kExit) == code(TASK | kSrch));

    // A task of the taskset made with cellSpursCreateTask is not one of its own: nothing to join (SRCH).
    write_task_elf(b, kElf + 0x3000, exiting_task(0x44));
    ps_write32(b, kArg, kOut + 0x300);
    REQUIRE(s.a.call("cellSpursCreateTask", kTs2, kId, kElf + 0x3000, 0, 0, 0, kArg) == 0);
    const u32 plain = ps_read32(b, kId);
    CHECK(s.a.call("cellSpursTryJoinTask2", kTs2, plain, kExit) == code(TASK | kSrch));
    REQUIRE(wait_for([&] { return s.report(3) == 1; }));
}

TEST_CASE("CellSpursTaskset2: a task that exited keeps its ID until it is joined; JoinTask2 waits for its task, a second "
          "one is BUSY (D-626)", "[hle][spurs][spurs-task2]")
{
    Task2Fixture s;
    const u32 w = s.create(0, waiting_task(0x55), "waiter", true);
    const u32 e = s.create(1, exiting_task(0x66), "exiter");
    s.run();
    REQUIRE(wait_for([&] { return s.report(1) == 1 && !s.task_bit(0x30, e); })); // ran and destroyed
    REQUIRE(wait_for([&] { return s.report(0) == 1 && s.task_bit(0x50, w); }));  // waiting
    // The exited task's ID is not given to the next task before its exit code was taken.
    const u32 c = s.create(2, exiting_task(0x77), "third");
    CHECK(c == 2);
    {
        Caller join(s.f, 1, "cellSpursJoinTask2", {kTs2, w, kExit + 4});
        REQUIRE(join.parked_on(lv2::WaitKind::EventFlag));
        CHECK(s.a.call("cellSpursJoinTask2", kTs2, w, kExit) == code(TASK | kBusy));
        CHECK(s.a.call("cellSpursTryJoinTask2", kTs2, w, kExit) == code(TASK | kBusy));
        REQUIRE(s.a.call("_cellSpursSendSignal", kTs2, w) == 0);
        join.join();
        CHECK(join.result == 0);
        CHECK(ps_read32(s.b, kExit + 4) == 0x55);
        CHECK(s.report(0, 16) == 2);
    }
    // An exit code pointer is needed (NULL_POINTER, libsre's first check).
    CHECK(s.a.call("cellSpursJoinTask2", kTs2, e, 0) == code(TASK | kNull));
    CHECK(s.a.call("cellSpursTryJoinTask2", kTs2, e, 0) == code(TASK | kNull));
    REQUIRE(s.a.call("cellSpursJoinTask2", kTs2, e, kExit) == 0);
    CHECK(ps_read32(s.b, kExit) == 0x66);
    REQUIRE(s.a.call("cellSpursJoinTask2", kTs2, c, kExit) == 0);
    CHECK(ps_read32(s.b, kExit) == 0x77);
    const u32 again = s.create(3, exiting_task(0x88), "again");
    CHECK(again == 0);
    REQUIRE(s.a.call("cellSpursJoinTask2", kTs2, again, kExit) == 0);
    CHECK(ps_read32(s.b, kExit) == 0x88);
    // Seventy at once, none joined before all exited: IDs 0..69, those from 64 on joined through the second event flag;
    // each exits with the code in its argument's second word.
    Program p;
    p.emit(as::ila(1, 0x3FFD0));
    p.emit(as::rotqbyi(20, 3, 4)); // the argument's word 1
    p.exit_with_register(20);
    write_task_elf(s.b, kElf + 0x1000 * 4, p);
    std::vector<u32> ids;
    for (u32 n = 0; n < 70; n++) {
        std::memset(s.b + kArg, 0, 16);
        ps_write32(s.b, kArg + 4, 0x100 + n);
        REQUIRE(s.a.call("cellSpursCreateTask2", kTs2, kId, kElf + 0x1000 * 4, kArg, 0) == 0);
        ids.push_back(ps_read32(s.b, kId));
        CHECK(ids.back() == n);
    }
    for (u32 n = 70; n-- > 0;) {
        REQUIRE(s.a.call("cellSpursJoinTask2", kTs2, ids[n], kExit) == 0);
        CHECK(ps_read32(s.b, kExit) == 0x100 + n);
    }
    CHECK(s.a.call("cellSpursTryJoinTask2", kTs2, 69, kExit) == code(TASK | kSrch));
}

TEST_CASE("cellSpursCreateTask2WithBinInfo: the ELF, context size and LS pattern from the bin info, the context in "
          "the buffer given (D-626)", "[hle][spurs][spurs-task2]")
{
    Task2Fixture s;
    u8* b = s.b;
    std::memset(b + kPrio, 0, 8);
    REQUIRE(s.a.call("cellSpursCreateTaskset", kSpurs, kTs1, 0, kPrio, 1) == 0);
    write_task_elf(b, kElf, waiting_task(0x99));
    std::memset(b + kBinInfo, 0, 32);
    ps_write64(b, kBinInfo, kElf);
    ps_write32(b, kBinInfo + 8, 0xC00);
    top_block_pattern(b, kBinInfo + 0x10);
    std::memset(b + kArg, 0, 16);
    ps_write32(b, kArg, kOut);
    const u32 name = kAttr2 + 0x80;
    std::strcpy(reinterpret_cast<char*>(b + name), "binned");
    const auto create = [&](u32 ts, u32 id, u32 info, u32 ctx) {
        return s.a.call("cellSpursCreateTask2WithBinInfo", ts, id, info, kArg, ctx, name, 0);
    };
    CHECK(create(kTs2, 0, kBinInfo, kCtx) == code(TASK | kNull));
    CHECK(create(0, kId, kBinInfo, kCtx) == code(TASK | kNull));
    CHECK(create(kTs2, kId, 0, kCtx) == code(TASK | kNull));
    CHECK(create(kTs2 + 4, kId, kBinInfo, kCtx) == code(TASK | kAlign));
    CHECK(create(kTs1, kId, kBinInfo, kCtx) == code(TASK | kPerm));
    CHECK(create(kTs2, kId, kUnmapped, kCtx) == code(TASK | kFault));
    CHECK(create(kTs2, kId, kBinInfo, 0) == code(TASK | kNull));         // a context asked for, no buffer
    CHECK(create(kTs2, kId, kBinInfo, kCtx + 0x40) == code(TASK | kAlign)); // 128-aligned from SDK 2.8
    ps_write32(b, kBinInfo + 8, 0x200);
    CHECK(create(kTs2, kId, kBinInfo, kCtx) == code(TASK | kInval)); // smaller than the execution context
    ps_write32(b, kBinInfo + 8, 0xC00);
    REQUIRE(create(kTs2, kId, kBinInfo, kCtx) == 0);
    const u32 id = ps_read32(b, kId);
    const u32 ti = kTs2 + 0x80 + 0x30 * id;
    CHECK(ps_read64(b, ti + 0x10) == (kElf | 4));
    CHECK(ps_read64(b, ti + 0x18) == (kCtx | 1)); // one LS block saved
    CHECK(ps_read8(b, ti + 0x2F) == 1);           // the pattern: the stack's block
    CHECK(std::string(reinterpret_cast<const char*>(b + kNames + 32 * id)) == "binned");
    s.run();
    REQUIRE(wait_for([&] { return s.report(0) == 1 && s.task_bit(0x50, id); })); // waits, its context saved
    REQUIRE(s.a.call("_cellSpursSendSignal", kTs2, id) == 0);
    REQUIRE(s.a.call("cellSpursJoinTask2", kTs2, id, kExit) == 0);
    CHECK(ps_read32(b, kExit) == 0x99);
    CHECK(s.report(0, 16) == 2);
}

TEST_CASE("cellSpursDestroyTaskset2: shut down and joined, its event flags destroyed; a JoinTask2 still waiting ends "
          "with SHUTDOWN (D-626)", "[hle][spurs][spurs-task2]")
{
    Task2Fixture s;
    u8* b = s.b;
    std::memset(b + kPrio, 0, 8);
    REQUIRE(s.a.call("cellSpursCreateTaskset", kSpurs, kTs1, 0, kPrio, 1) == 0);
    const u32 w = s.create(0, waiting_task(1), "forever", true);
    s.run();
    REQUIRE(wait_for([&] { return s.report(0) == 1 && s.task_bit(0x50, w); }));
    const u32 f1 = ps_read32(b, kTs2 + 0x1898), f2 = ps_read32(b, kTs2 + 0x189C);
    REQUIRE(s.f.kernel->event_flag(f1) != nullptr);
    REQUIRE(s.f.kernel->event_flag(f2) != nullptr);
    CHECK(s.a.call("cellSpursDestroyTaskset2", 0) == code(TASK | kNull));
    CHECK(s.a.call("cellSpursDestroyTaskset2", kTs2 + 8) == code(TASK | kAlign));
    CHECK(s.a.call("cellSpursDestroyTaskset2", kUnmapped) == code(TASK | kFault));
    CHECK(s.a.call("cellSpursDestroyTaskset2", kTs1) == code(TASK | kPerm));
    {
        Caller join(s.f, 1, "cellSpursJoinTask2", {kTs2, w, kExit});
        REQUIRE(join.parked_on(lv2::WaitKind::EventFlag));
        REQUIRE(s.a.call("cellSpursDestroyTaskset2", kTs2) == 0);
        join.join();
        CHECK(join.result == code(TASK | kShutdown));
    }
    CHECK(ps_read32(b, kTs2 + 0x1898) == 0);
    CHECK(ps_read32(b, kTs2 + 0x189C) == 0);
    CHECK(s.f.kernel->event_flag(f1) == nullptr);
    CHECK(s.f.kernel->event_flag(f2) == nullptr);
    CHECK(ps_read8(b, kSpurs + 0x80 + s.wid) == 0); // the workload is gone (NON_EXISTENT)
    // Made again at the same address: no task of the old one is reserved; its tasks run and are joined.
    REQUIRE(s.a.call("cellSpursCreateTaskset2", kSpurs, kTs2, kTs2Attr) == 0);
    s.wid = ps_read32(b, kTs2 + 0x74);
    const u32 again = s.create(1, exiting_task(5), "again");
    CHECK(again == 0);
    s.run();
    REQUIRE(s.a.call("cellSpursJoinTask2", kTs2, again, kExit) == 0);
    CHECK(ps_read32(b, kExit) == 5);
    REQUIRE(s.a.call("cellSpursDestroyTaskset2", kTs2) == 0);
}

// ------------------------------------------------------------------------------------------------- D-627

TEST_CASE("cellSpursTaskExitCodeGet and TryGet: their checks; a container cellSpursCreateTaskWithAttribute binds to a "
          "task gets its exit code, which Get waits for (D-627)", "[hle][spurs][spurs-task2]")
{
    Task2Fixture s;
    u8* b = s.b;
    constexpr u32 kSaveConfig = kTaskAttr + 0x100;
    std::memset(b + kContainer, 0xA5, 128);
    REQUIRE(s.a.call("cellSpursTaskExitCodeInitialize", kContainer) == 0);
    for (const char* fn : {"cellSpursTaskExitCodeGet", "cellSpursTaskExitCodeTryGet"}) {
        INFO(fn);
        CHECK(s.a.call(fn, 0, kExit) == code(TASK | kNull));
        CHECK(s.a.call(fn, kContainer, 0) == code(TASK | kNull));
        CHECK(s.a.call(fn, kContainer + 0x40, kExit) == code(TASK | kAlign));
        CHECK(s.a.call(fn, kUnmapped, kExit) == code(TASK | kFault));
        CHECK(s.a.call(fn, kContainer, kUnmapped) == code(TASK | kFault));
        // No task started with it: nothing will store a code.
        CHECK(s.a.call(fn, kContainer, kExit) == code(TASK | kStat));
    }

    // A task created with the container: bound (the status' bit 0), an exit callback (the ELF word's bit 0) whose
    // slot names the taskset's exit code handler and the container.
    write_task_elf(b, kElf, exiting_task(0x77));
    std::memset(b + kArg, 0, 16);
    ps_write32(b, kArg, kOut);
    REQUIRE(s.a.call("_cellSpursTaskAttributeInitialize", kTaskAttr, 1, 0x330000, kElf, 0, kArg) == 0);
    REQUIRE(s.a.call("cellSpursTaskAttributeSetExitCodeContainer", kTaskAttr, kContainer) == 0);
    CHECK(s.a.call("cellSpursCreateTaskWithAttribute", kTs2, kId, kTaskAttr) == 0);
    const u32 id = ps_read32(b, kId);
    CHECK(ps_read32(b, kContainer + 0x20) == 3); // bound, then started (libsre's two steps)
    CHECK(ps_read64(b, kContainer + 0x50) == kTs2);
    CHECK(ps_read64(b, kTs2 + 0x80 + 0x30 * id + 0x10) == (kElf | 1));
    CHECK(ps_read64(b, kTs2 + 0x1980 + 16 * id) == ps_read64(b, kTs2 + 0x1900));
    CHECK(ps_read64(b, kTs2 + 0x1988 + 16 * id) == kContainer);
    CHECK(s.a.call("cellSpursTaskExitCodeTryGet", kContainer, kExit) == code(TASK | kAgain)); // not run yet
    {
        Caller get(s.f, 1, "cellSpursTaskExitCodeGet", {kContainer, kExit + 4});
        CHECK(get.blocked());
        s.run();
        get.join();
        CHECK(get.result == 0);
        CHECK(ps_read32(b, kExit + 4) == 0x77);
    }
    CHECK(line_word(b, kContainer + 0x20) == 0); // the code taken: the status word cleared
    CHECK(ps_read32(b, kContainer + 0x28) == 0x77);
    CHECK(s.a.call("cellSpursTaskExitCodeTryGet", kContainer, kExit) == code(TASK | kStat)); // taken once
    CHECK(s.a.call("cellSpursTaskExitCodeGet", kContainer, kExit) == code(TASK | kStat));
    CHECK(s.report(0) == 1);

    // Bound to a second task (one that waits for a signal first): the old code is forgotten until the new one's comes.
    write_task_elf(b, kElf + 0x1000, waiting_task(0x78));
    ps_write32(b, kArg, kOut + 0x100);
    ps_write32(b, kSaveConfig + 0, kCtx);
    ps_write32(b, kSaveConfig + 4, 0xC00);
    ps_write32(b, kSaveConfig + 8, kPattern);
    top_block_pattern(b, kPattern);
    REQUIRE(s.a.call("_cellSpursTaskAttributeInitialize", kTaskAttr, 1, 0x330000, kElf + 0x1000, kSaveConfig, kArg) == 0);
    REQUIRE(s.a.call("cellSpursTaskAttributeSetExitCodeContainer", kTaskAttr, kContainer) == 0);
    REQUIRE(s.a.call("cellSpursCreateTaskWithAttribute", kTs2, kId, kTaskAttr) == 0);
    const u32 waiter = ps_read32(b, kId);
    CHECK(line_word(b, kContainer + 0x20) == 3);
    CHECK(s.a.call("cellSpursTaskExitCodeTryGet", kContainer, kExit) == code(TASK | kAgain));
    REQUIRE(wait_for([&] { return s.report(1) == 1 && s.task_bit(0x50, waiter); }));
    CHECK(s.a.call("cellSpursTaskExitCodeTryGet", kContainer, kExit) == code(TASK | kAgain));
    // Another waiter's mark (bit 16 or 17): BUSY.
    ps_write32(b, kContainer + 0x20, 0x20003);
    CHECK(s.a.call("cellSpursTaskExitCodeTryGet", kContainer, kExit) == code(TASK | kBusy));
    ps_write32(b, kContainer + 0x20, 3);
    REQUIRE(s.a.call("_cellSpursSendSignal", kTs2, waiter) == 0);
    REQUIRE(s.a.call("cellSpursTaskExitCodeGet", kContainer, kExit) == 0);
    CHECK(ps_read32(b, kExit) == 0x78);
    CHECK(s.report(1, 16) == 2);
    // A container that is not guest memory: FAULT; on a CellSpursTaskset: PERM.
    ps_write64(b, kTaskAttr + 0x40, kUnmapped);
    CHECK(s.a.call("cellSpursCreateTaskWithAttribute", kTs2, kId, kTaskAttr) == code(TASK | kFault));
    std::memset(b + kPrio, 0, 8);
    REQUIRE(s.a.call("cellSpursCreateTaskset", kSpurs, kTs1, 0, kPrio, 1) == 0);
    ps_write64(b, kTaskAttr + 0x40, kContainer);
    CHECK(s.a.call("cellSpursCreateTaskWithAttribute", kTs1, kId, kTaskAttr) == code(TASK | kPerm));
}

// ------------------------------------------------------------------------------------------------- D-628

namespace {

// CellSyncLFQueue's fields (RPCS3 cellSync.h).
constexpr u32 kH1 = 0x00, kH3 = 0x04, kH4 = 0x06, kH5 = 0x08, kH7 = 0x0C, kH8 = 0x0E, kBs = 0x20, kPush2 = 0x30,
              kHs1 = 0x32, kPop2 = 0x50, kHs2 = 0x52, kEqId = 0x7C;
constexpr u32 SPU2SPU = 0, SPU2PPU = 1, PPU2SPU = 2, ANY2ANY = 3;

void put_entry(u8* b, u32 addr, u32 v)
{
    for (u32 i = 0; i < 16; i += 4) ps_write32(b, addr + i, v);
}

// An SPU's update of a line: GETLLAR, the change, PUTLLC, again until it lands (test_sync.cpp's).
template <class Fn>
bool spu_update(u8* b, u32 addr, Fn fn)
{
    alignas(16) u8 line[PS_RES_LINE_SIZE];
    alignas(16) u8 data[PS_RES_LINE_SIZE];
    for (;;) {
        const u64 version = ps_res_acquire(b, addr, line);
        std::memcpy(data, line, sizeof data);
        if (!fn(data)) return false;
        if (ps_res_store_line(b, addr, version, line, data)) return true;
    }
}

// SPU code's side of the queue at `q` (entries at `entries`), in order: a pop of a PPU2SPU queue, a push into an
// SPU2PPU one (test_sync.cpp's). False when there is no entry (no room).
bool spu_pop(u8* b, u32 q, u32 entries, u32 depth, u32& value)
{
    s32 p = -1;
    spu_update(b, q, [&](u8* l) {
        const s32 next = static_cast<s16>(ps_read16(l, kH4));
        s32 avail = ps_read16(l, kH5) - next;
        if (avail < 0) avail += 2 * static_cast<s32>(depth);
        if (avail <= 0) return false;
        p = next;
        ps_write16(l, kH4, static_cast<u16>(p + 1 >= static_cast<s32>(2 * depth) ? 0 : p + 1));
        return true;
    });
    if (p < 0) return false;
    value = ps_read32(b, entries + 16 * (static_cast<u32>(p) % depth));
    spu_update(b, q, [&](u8* l) {
        ps_write16(l, kH1, static_cast<u16>(p + 1 >= static_cast<s32>(2 * depth) ? 0 : p + 1));
        return true;
    });
    return true;
}

bool spu_push(u8* b, u32 q, u32 entries, u32 depth, u32 value)
{
    s32 p = -1;
    spu_update(b, q, [&](u8* l) {
        const s32 next = static_cast<s16>(ps_read16(l, kH8));
        s32 used = next - ps_read16(l, kH1);
        if (used < 0) used += 2 * static_cast<s32>(depth);
        if (used >= static_cast<s32>(depth)) return false;
        p = next;
        ps_write16(l, kH8, static_cast<u16>(p + 1 >= static_cast<s32>(2 * depth) ? 0 : p + 1));
        return true;
    });
    if (p < 0) return false;
    put_entry(b, entries + 16 * (static_cast<u32>(p) % depth), value);
    spu_update(b, q, [&](u8* l) {
        ps_write16(l, kH5, static_cast<u16>(p + 1 >= static_cast<s32>(2 * depth) ? 0 : p + 1));
        return true;
    });
    return true;
}

// SPU code that found the queue empty (or full) and waits: its ID at the tail of the side's ring (the pack's tail moves
// on), then the wait for a signal (here the task's own syscall).
void spu_wait_in_ring(u8* b, u32 q, u32 pack, u32 ring, u32 id)
{
    spu_update(b, q, [&](u8* l) {
        const u16 p = ps_read16(l, pack);
        const u32 tail = p & 0x1F;
        ps_write16(l, ring + 2 * (tail >= 15 ? tail - 15 : tail), static_cast<u16>(id));
        ps_write16(l, pack, static_cast<u16>((p & ~0x1Fu) | (tail == 0x1D ? 0 : tail + 1)));
        return true;
    });
}

// sys_spu_thread_throw_event(port, 0, 0) from an SPU of `spurs`, as the native workers deliver it (test_spurs_queue.cpp's).
void throw_event(Fixture& f, u32 spurs, u32 port)
{
    u32 queue_id = 0;
    {
        hle::spurs::SpursHost& h = hle::spurs::host();
        std::lock_guard lock(h.mutex);
        queue_id = h.instances.at(spurs).spu_ports.at(static_cast<u8>(port));
    }
    const auto q = f.kernel->event_queue(queue_id);
    REQUIRE(q != nullptr);
    REQUIRE(q->send(lv2::Event{0xFFFFFFFF53505501ull, 0, u64{port} << 32, 0}) == lv2::CELL_OK);
}

} // namespace

TEST_CASE("_cellSpursLFQueuePushBody and PopBody: a taskset's queues pushed and popped by the PPU against SPU code; a "
          "waiting task taken from the ring is signalled through _cellSpursSendSignal (D-628)",
    "[hle][spurs][spurs-task2]")
{
    Task2Fixture s;
    u8* b = s.b;
    for (const char* fn : {"_cellSpursLFQueuePushBody", "_cellSpursLFQueuePopBody"}) {
        INFO(fn);
        CHECK(s.a.call(fn, 0, kSrc, 0) == code(TASK | kNull));
        CHECK(s.a.call(fn, kLfq, 0, 0) == code(TASK | kNull));
        CHECK(s.a.call(fn, kLfq + 64, kSrc, 0) == code(TASK | kAlign));
        // libsre's order: PushBody looks at the queue's alignment before NULL, PopBody after (D-914).
        CHECK(s.a.call(fn, kLfq + 64, 0, 0) ==
            code(TASK | (std::string_view(fn) == "_cellSpursLFQueuePushBody" ? kAlign : kNull)));
        CHECK(s.a.call(fn, kUnmapped, kSrc, 0) == code(TASK | kFault));
    }

    // PPU2SPU: the PPU pushes, SPU code pops.
    std::memset(b + kLfq, 0, 128);
    REQUIRE(s.a.call("_cellSpursLFQueueInitialize", kTs2, kLfq, kLfqBuf, 16, 4, PPU2SPU) == 0);
    for (u32 v = 1; v <= 4; v++) {
        put_entry(b, kSrc, v);
        REQUIRE(s.a.call("_cellSpursLFQueuePushBody", kLfq, kSrc, v % 2) == 0);
        CHECK(ps_read32(b, kLfqBuf + 16 * (v - 1)) == v);
    }
    CHECK(s.a.call("_cellSpursLFQueuePushBody", kLfq, kSrc, 0) == code(TASK | kAgain));
    CHECK(s.a.call("_cellSpursLFQueuePushBody", kLfq, kSrc + 8, 0) == code(TASK | kAgain)); // no buffer alignment
    CHECK(s.a.call("_cellSpursLFQueuePopBody", kLfq, kDst, 0) == code(TASK | kPerm));
    for (u32 v = 1; v <= 4; v++) {
        u32 value = 0;
        REQUIRE(spu_pop(b, kLfq, kLfqBuf, 4, value));
        CHECK(value == v);
    }
    // A blocked push of a full queue goes on when SPU code pops (another thread here).
    for (u32 v = 5; v <= 8; v++) REQUIRE(s.a.call("_cellSpursLFQueuePushBody", kLfq, kSrc, 0) == 0);
    {
        put_entry(b, kSrc + 0x40, 9);
        Caller push(s.f, 1, "_cellSpursLFQueuePushBody", {kLfq, kSrc + 0x40, 1});
        CHECK(push.blocked());
        u32 value = 0;
        REQUIRE(spu_pop(b, kLfq, kLfqBuf, 4, value));
        push.join();
        CHECK(push.result == 0);
    }
    for (u32 n = 0; n < 4; n++) {
        u32 value = 0;
        REQUIRE(spu_pop(b, kLfq, kLfqBuf, 4, value));
    }
    // A task finds the queue empty and waits in the push side's ring; the PPU's push takes it from there and signals it
    // through _cellSpursSendSignal(taskset, its ID): it resumes and ends.
    const u32 w = s.create(0, waiting_task(0x31), "popper", true);
    s.run();
    REQUIRE(wait_for([&] { return s.report(0) == 1 && s.task_bit(0x50, w); }));
    spu_wait_in_ring(b, kLfq, kPush2, kHs1, w);
    put_entry(b, kSrc, 10);
    REQUIRE(s.a.call("_cellSpursLFQueuePushBody", kLfq, kSrc, 1) == 0);
    REQUIRE(s.a.call("cellSpursJoinTask2", kTs2, w, kExit) == 0);
    CHECK(ps_read32(b, kExit) == 0x31);
    CHECK(s.report(0, 16) == 2);
    CHECK((line_half(b, kLfq + kPush2) >> 10 & 0x1F) == (line_half(b, kLfq + kPush2) & 0x1F)); // the ring is empty
    // A waiter that is no task of the taskset: _cellSpursSendSignal's SRCH is the push's answer (the entry is in).
    spu_wait_in_ring(b, kLfq, kPush2, kHs1, 77);
    CHECK(s.a.call("_cellSpursLFQueuePushBody", kLfq, kSrc, 1) == code(TASK | kSrch));
    CHECK(line_half(b, kLfq + kH5) == 3);

    // SPU2PPU: SPU code pushes, the PPU pops; the pop completion wakes a task waiting for room.
    std::memset(b + kLfq2, 0, 128);
    REQUIRE(s.a.call("_cellSpursLFQueueInitialize", kTs2, kLfq2, kLfqBuf + 0x100, 16, 2, SPU2PPU) == 0);
    CHECK(s.a.call("_cellSpursLFQueuePopBody", kLfq2, kDst, 0) == code(TASK | kAgain));
    CHECK(s.a.call("_cellSpursLFQueuePushBody", kLfq2, kSrc, 0) == code(TASK | kPerm));
    REQUIRE(spu_push(b, kLfq2, kLfqBuf + 0x100, 2, 21));
    REQUIRE(spu_push(b, kLfq2, kLfqBuf + 0x100, 2, 22));
    const u32 w2 = s.create(1, waiting_task(0x32), "pusher", true);
    REQUIRE(wait_for([&] { return s.report(1) == 1 && s.task_bit(0x50, w2); }));
    spu_wait_in_ring(b, kLfq2, kPop2, kHs2, w2);
    REQUIRE(s.a.call("_cellSpursLFQueuePopBody", kLfq2, kDst, 1) == 0);
    CHECK(ps_read32(b, kDst) == 21);
    REQUIRE(s.a.call("cellSpursJoinTask2", kTs2, w2, kExit) == 0);
    CHECK(ps_read32(b, kExit) == 0x32);
    {
        Caller pop(s.f, 1, "_cellSpursLFQueuePopBody", {kLfq2, kDst + 0x40, 1});
        REQUIRE(wait_for([&] { return pop.done(); }));
        CHECK(pop.result == 0);
        CHECK(ps_read32(b, kDst + 0x40) == 22);
    }
    {
        Caller pop(s.f, 1, "_cellSpursLFQueuePopBody", {kLfq2, kDst + 0x40, 1});
        CHECK(pop.blocked()); // empty
        REQUIRE(spu_push(b, kLfq2, kLfqBuf + 0x100, 2, 23));
        pop.join();
        CHECK(pop.result == 0);
        CHECK(ps_read32(b, kDst + 0x40) == 23);
    }

    // ANY2ANY: both sides from PPU threads (cellSync's second forms).
    std::memset(b + kLfq2, 0, 128);
    REQUIRE(s.a.call("_cellSpursLFQueueInitialize", kTs2, kLfq2, kLfqBuf + 0x100, 16, 2, ANY2ANY) == 0);
    for (u32 v = 30; v < 36; v++) {
        put_entry(b, kSrc, v);
        REQUIRE(s.a.call("_cellSpursLFQueuePushBody", kLfq2, kSrc, 1) == 0);
        REQUIRE(s.a.call("_cellSpursLFQueuePopBody", kLfq2, kDst, 1) == 0);
        CHECK(ps_read32(b, kDst) == v);
    }
    CHECK(s.a.call("_cellSpursLFQueuePopBody", kLfq2, kDst, 0) == code(TASK | kAgain));
}

TEST_CASE("_cellSpursLFQueuePushBody: a queue of the whole instance (IWL) wakes the workload waiting in its ring with "
          "cellSpursSendWorkloadSignal (D-628)", "[hle][spurs][spurs-task2]")
{
    Task2Fixture s(false);
    u8* b = s.b;
    std::memset(b + kLfq, 0, 128);
    REQUIRE(s.a.call("_cellSpursLFQueueInitialize", kSpurs | 1, kLfq, kLfqBuf, 16, 2, PPU2SPU) == 0);
    CHECK(ps_read64(b, kLfq + 0x70) == (kSpurs | 1));
    CHECK((line_half(b, kSpurs + 0x70) & (0x8000 >> s.wid)) == 0);
    spu_wait_in_ring(b, kLfq, kPush2, kHs1, s.wid); // the taskset's workload stands for one waiting
    put_entry(b, kSrc, 1);
    REQUIRE(s.a.call("_cellSpursLFQueuePushBody", kLfq, kSrc, 0) == 0);
    CHECK((line_half(b, kSpurs + 0x70) & (0x8000 >> s.wid)) != 0); // its workload signal
    // A workload that does not exist: cellSpursSendWorkloadSignal's SRCH is the answer.
    spu_wait_in_ring(b, kLfq, kPush2, kHs1, 9);
    CHECK(s.a.call("_cellSpursLFQueuePushBody", kLfq, kSrc, 0) == code(POLICY | kSrch));
}

TEST_CASE("cellSpursLFQueueAttachLv2EventQueue and Detach: an lv2 queue on a dynamic SPU port of the queue's SPURS, "
          "which wakes a PPU blocked in the pointer functions with useEventQueue (D-628)", "[hle][spurs][spurs-task2]")
{
    Task2Fixture s(false);
    u8* b = s.b;
    for (const char* fn : {"cellSpursLFQueueAttachLv2EventQueue", "cellSpursLFQueueDetachLv2EventQueue"}) {
        INFO(fn);
        CHECK(s.a.call(fn, 0) == code(TASK | kNull));
        CHECK(s.a.call(fn, kLfq + 0x20) == code(TASK | kAlign));
        CHECK(s.a.call(fn, kUnmapped) == code(TASK | kFault));
    }
    std::memset(b + kLfq2, 0, 128);
    REQUIRE(s.a.call("_cellSpursLFQueueInitialize", kTs2, kLfq2, kLfqBuf, 16, 2, SPU2SPU) == 0);
    CHECK(s.a.call("cellSpursLFQueueAttachLv2EventQueue", kLfq2) == code(TASK | kPerm)); // no PPU side
    std::memset(b + kLfq, 0, 128);
    REQUIRE(s.a.call("_cellSpursLFQueueInitialize", kTs2, kLfq, kLfqBuf, 16, 2, PPU2SPU) == 0);
    CHECK(s.a.call("cellSpursLFQueueDetachLv2EventQueue", kLfq) == code(TASK | kStat)); // none attached

    REQUIRE(s.a.call("cellSpursLFQueueAttachLv2EventQueue", kLfq) == 0);
    const u32 port = ps_read32(b, kLfq + kBs), eq = ps_read32(b, kLfq + kEqId);
    CHECK(port >= 0x10);
    CHECK(port <= 0x3F);
    REQUIRE(s.f.kernel->event_queue(eq) != nullptr);
    {
        hle::spurs::SpursHost& h = hle::spurs::host();
        std::lock_guard lock(h.mutex);
        CHECK(h.instances.at(kSpurs).spu_ports.at(static_cast<u8>(port)) == eq);
    }
    CHECK((ps_read64(b, kSpurs + 0xDA8) >> port & 1) == 0); // a port SPURS took for itself: not in spuPortBits
    CHECK(s.a.call("cellSpursLFQueueAttachLv2EventQueue", kLfq) == code(TASK | kStat)); // one already

    // Full: a PPU thread's GetPushPointer with the event queue sleeps on it until SPU code pops and throws its event.
    for (u32 v = 0; v < 2; v++) REQUIRE(s.a.call("_cellSpursLFQueuePushBody", kLfq, kSrc, 0) == 0);
    {
        Caller get(s.f, 1, "_cellSyncLFQueueGetPushPointer", {kLfq, kOutWord, 1, 1});
        REQUIRE(get.parked_on(lv2::WaitKind::EventQueue));
        u32 value = 0;
        REQUIRE(spu_pop(b, kLfq, kLfqBuf, 2, value));
        CHECK(get.blocked());
        throw_event(s.f, kSpurs, port);
        get.join();
        CHECK(get.result == 0);
        CHECK(ps_read32(b, kOutWord) == 2);
    }
    REQUIRE(s.a.call("_cellSyncLFQueueCompletePushPointer", kLfq, 2, 0) == 0);
    // Detach: BUSY while a PPU thread waits on it (isBlocking 3 marks its side's event state), then gone.
    {
        Caller get(s.f, 1, "_cellSyncLFQueueGetPushPointer", {kLfq, kOutWord, 3, 1});
        REQUIRE(get.parked_on(lv2::WaitKind::EventQueue));
        CHECK(line_half(b, kLfq + kH7) == 3);
        CHECK(s.a.call("cellSpursLFQueueDetachLv2EventQueue", kLfq) == code(TASK | kBusy));
        u32 value = 0;
        REQUIRE(spu_pop(b, kLfq, kLfqBuf, 2, value));
        throw_event(s.f, kSpurs, port);
        get.join();
        CHECK(get.result == 0);
    }
    REQUIRE(s.a.call("cellSpursLFQueueDetachLv2EventQueue", kLfq) == 0);
    CHECK(ps_read32(b, kLfq + kBs) == 0xFFFFFFFF);
    CHECK(ps_read32(b, kLfq + kEqId) == 0);
    CHECK(s.f.kernel->event_queue(eq) == nullptr);
    {
        hle::spurs::SpursHost& h = hle::spurs::host();
        std::lock_guard lock(h.mutex);
        CHECK_FALSE(h.instances.at(kSpurs).spu_ports.contains(static_cast<u8>(port)));
    }
    CHECK(s.a.call("cellSpursLFQueueDetachLv2EventQueue", kLfq) == code(TASK | kStat));

    // A queue of the whole instance (IWL), ANY2ANY: the same, its m_bs back to what Initialize left.
    std::memset(b + kLfq2, 0, 128);
    REQUIRE(s.a.call("_cellSpursLFQueueInitialize", kSpurs | 1, kLfq2, kLfqBuf, 16, 2, ANY2ANY) == 0);
    REQUIRE(s.a.call("cellSpursLFQueueAttachLv2EventQueue", kLfq2) == 0);
    CHECK(ps_read32(b, kLfq2 + kBs) >= 0x10);
    REQUIRE(s.a.call("cellSpursLFQueueDetachLv2EventQueue", kLfq2) == 0);
    CHECK(ps_read32(b, kLfq2 + kBs) == 0xFFFF0000);
    CHECK(line_half(b, kLfq2 + kH3) == 0);
}

// ------------------------------------------------------------------------------------------------- D-629

TEST_CASE("cellSpursGetJobPipelineInfo: its checks; an SPU thread of the instance's, read through lv2 (D-914)",
    "[hle][spurs][spurs-task2]")
{
    Task2Fixture s(false);
    u8* b = s.b;
    std::memset(b + kPrio, 0, 8);
    ps_write64(b, kCmds, 0x7F); // END
    REQUIRE(s.a.call("cellSpursCreateJobChain", kSpurs, kChain, kCmds, 0x80, 4, kPrio, 1, 1, 4, 5) == 0);
    // Its second argument is an SPU thread ID: one of the instance's (cellSpursGetSpuThreadId's list), else INVAL.
    constexpr u32 kThreads = kOutWord + 0x10;
    ps_write32(b, kThreads + 0x20, 8);
    REQUIRE(s.a.call("cellSpursGetSpuThreadId", kSpurs, kThreads, kThreads + 0x20) == 0);
    REQUIRE(ps_read32(b, kThreads + 0x20) == 1);
    const u32 thread = ps_read32(b, kThreads);
    CHECK(s.a.call("cellSpursGetJobPipelineInfo", 0, thread, kOutWord) == code(JOB | kNull));
    CHECK(s.a.call("cellSpursGetJobPipelineInfo", kSpurs, thread, 0) == code(JOB | kNull));
    CHECK(s.a.call("cellSpursGetJobPipelineInfo", kSpurs + 0x40, thread, kOutWord) == code(JOB | kAlign));
    CHECK(s.a.call("cellSpursGetJobPipelineInfo", kUnmapped, thread, kOutWord) == code(JOB | kFault));
    CHECK(s.a.call("cellSpursGetJobPipelineInfo", kSpurs, thread + 1, kOutWord) == code(JOB | kInval));
    // The instance's thread: libsre reads its local store; a native SPURS has no such SPU thread (lv2's ESRCH).
    ps_write32(b, kOutWord, 0x5A5A5A5A);
    CHECK(s.a.call("cellSpursGetJobPipelineInfo", kSpurs, thread, kOutWord) == r3_of(lv2::CELL_ESRCH));
    CHECK(ps_read32(b, kOutWord) == 0x5A5A5A5A); // nothing written
    REQUIRE(s.a.call("cellSpursShutdownJobChain", kChain) == 0);
    REQUIRE(s.a.call("cellSpursJoinJobChain", kChain) == 0);
}

TEST_CASE("cellSpursRemoveSystemWorkloadForUtility: a handle of libsre's system utility workload, never registered "
          "natively (D-914)", "[hle][spurs][spurs-task2]")
{
    constexpr u32 kHandle = kData + 0x18000;
    Fixture f(config(false));
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    CHECK(a.call("cellSpursRemoveSystemWorkloadForUtility", 0) == code(CORE | kNull));
    CHECK(a.call("cellSpursRemoveSystemWorkloadForUtility", kHandle + 0x40) == code(CORE | kAlign));
    CHECK(a.call("cellSpursRemoveSystemWorkloadForUtility", kUnmapped) == code(CORE | kNull));
    std::memset(b + kHandle, 0, 0x100);
    CHECK(a.call("cellSpursRemoveSystemWorkloadForUtility", kHandle) == code(CORE | kInval)); // not marked
    ps_write64(b, kHandle, 0xCE3F7FF5653795B9ull);
    CHECK(a.call("cellSpursRemoveSystemWorkloadForUtility", kHandle) == code(CORE | kInval)); // marked, not registered
    CHECK(ps_read64(b, kHandle) == 0xCE3F7FF5653795B9ull);
}
