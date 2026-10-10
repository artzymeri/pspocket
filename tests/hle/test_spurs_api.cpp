// cellSpurs up to RPCS3's API (D-171), on the native scheduler: every function registered under its NID; initialize,
// information, the attribute's container, the SPU GUID decoder (libsre's); workload priorities, contention, idle SPU
// requests, signals, data and ready counts; the workload flag receivers; shutdown, wait and remove with a
// shutdown-completion hook called on the event helper thread; traces and exception handler flags; CellSpursTaskset2
// and the taskset accessors; event flag getters, clear and try-wait, the barrier's taskset, the SPURS LF queue; and a
// SPURS2 instance whose workloads 16..31 run under kernel 2's selection. Everything is synthetic (hand-assembled SPU
// code, a few PPU instructions for the hook): no game data.

#include "hle_test_util.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include "pspocket/core/reservation.h"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/spu/context.h"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::spu::as;

namespace {

constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kWattr = kData + 0x4000, kTsAttr = kData + 0x4400,
              kPrio = kData + 0x4800, kOut = kData + 0x4900, kTs = kData + 0x6000, kTs2 = kData + 0x9000,
              kFlag = kData + 0xB000, kBarrier = kData + 0xB080, kLfq = kData + 0xB100, kLfqBuf = kData + 0xB200,
              kNames = kData + 0xC000, kHookCode = kData + 0xD000, kHookOpd = kData + 0xD100, kHookOut = kData + 0xD200,
              kTrace = kData + 0xE000, kPm = kData + 0xF000, kElf = kData + 0x10000, kTaskOut = kData + 0x14000,
              kId = kData + 0x14800, kArg = kData + 0x14900, kRecord = kData + 0x15000;
constexpr u32 kBuf = 0x3800; // SPU code's LS buffer for DMA

u64 code(u32 c) { return r3_of_code(c); }
constexpr u32 CORE = 0x80410700, PM = 0x80410800, TASK = 0x80410900;
constexpr u32 kNull = 0x11, kAlign = 0x10, kInval = 0x02, kSrch = 0x05, kStat = 0x0F, kBusy = 0x0A, kPerm = 0x09,
              kFault = 0x0D;

bool wait_for(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return true;
}

// SPU code, assembled at its LS address (a task at 0x3000, a policy module at 0xA00).
struct Program {
    std::vector<u32> code;
    void emit(u32 insn) { code.push_back(insn); }
    void load32(u32 rt, u32 v)
    {
        emit(as::ilhu(rt, static_cast<s32>(v >> 16)));
        emit(as::iohl(rt, static_cast<s32>(v & 0xFFFF)));
    }
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
    // Stores register `value`'s quadword at EA register `ea_reg` + `offset`.
    void put(u32 value, u32 ea_reg, s32 offset)
    {
        emit(as::il(71, kBuf));
        emit(as::stqd(value, 0, 71));
        emit(as::ai(72, ea_reg, offset));
        dma16(PS_MFC_PUT, 71, 72);
    }
    void syscall(u32 n)
    {
        emit(as::il(3, static_cast<s32>(n)));
        emit(as::ila(5, 0xA70));
        emit(as::bisl(0, 5));
    }
};

// A task ELF at `ea`: one read-only PT_LOAD at 0x3000 with the program.
void write_task_elf(u8* base, u32 ea, const Program& p)
{
    const u32 size = static_cast<u32>(p.code.size() * 4);
    std::memset(base + ea, 0, 0x100 + size + 16);
    static constexpr u8 kIdent[16] = {0x7F, 'E', 'L', 'F', 1, 2, 1, 0};
    std::memcpy(base + ea, kIdent, 16);
    ps_write16(base, ea + 0x10, 2);
    ps_write16(base, ea + 0x12, 0x17);
    ps_write32(base, ea + 0x14, 1);
    ps_write32(base, ea + 0x18, 0x3000);
    ps_write32(base, ea + 0x1C, 0x34);
    ps_write16(base, ea + 0x28, 0x34);
    ps_write16(base, ea + 0x2A, 0x20);
    ps_write16(base, ea + 0x2C, 1);
    const u32 ph = ea + 0x34;
    ps_write32(base, ph + 0x00, 1);
    ps_write32(base, ph + 0x04, 0x100);
    ps_write32(base, ph + 0x08, 0x3000);
    ps_write32(base, ph + 0x0C, 0x3000);
    ps_write32(base, ph + 0x10, size);
    ps_write32(base, ph + 0x14, (size + 15) & ~15u);
    ps_write32(base, ph + 0x18, 5);
    ps_write32(base, ph + 0x1C, 0x80);
    for (usize i = 0; i < p.code.size(); i++) ps_write32(base, ea + 0x100 + 4 * static_cast<u32>(i), p.code[i]);
}

// A task that writes its 16-byte argument to the EA in the argument's first word, then exits.
Program marker_task()
{
    Program p;
    p.emit(as::il(10, kBuf));
    p.emit(as::stqd(3, 0, 10));
    p.dma16(PS_MFC_PUT, 10, 3);
    p.syscall(0);
    return p;
}

// Guest code the HLE calls back into (the shutdown-completion hook) runs on the PPU interpreter while this lives; the
// policy before it comes back after.
struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// What the loader and the game's CRT do first: link cellSpurs' internal functions (the event helper), initialize TLS.
struct LinkedFixture : Fixture {
    LinkedFixture()
    {
        REQUIRE(hle->link_internal_functions({"cellSpurs"}));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

// One SPURS1 instance of `nspus` SPUs.
void init_spurs(GuestThread& a, u32 nspus = 1)
{
    REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, nspus, 200, 2016, 0) == 0);
    REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
}

// A custom workload whose policy module (a `bi $lr`) only returns; priorities `prio` (0: never on that SPU).
u32 add_idle_workload(GuestThread& a, u8* b, std::initializer_list<u8> prio, u32 max_contention = 1)
{
    ps_write32(b, kPm, as::bi(0));
    std::memset(b + kPrio, 0, 8);
    u32 i = 0;
    for (const u8 p : prio) ps_write8(b, kPrio + i++, p);
    REQUIRE(a.call("cellSpursAddWorkload", kSpurs, kId, kPm, 0x10, u64{0x5150}, kPrio, 1, max_contention) == 0);
    return ps_read32(b, kId);
}

} // namespace

TEST_CASE("cellSpurs (D-171): every function RPCS3 implements is registered under its NID", "[hle][spurs][spurs-api]")
{
    for (const char* name : {"cellSpursInitialize", "cellSpursInitializeWithAttribute2",
             "cellSpursAttributeSetMemoryContainerForSpuThread", "cellSpursGetInfo", "cellSpursGetNumSpuThread",
             "cellSpursGetSpuThreadGroupId", "cellSpursGetSpuThreadId", "cellSpursGetSpuGuid", "cellSpursSetMaxContention",
             "cellSpursSetPriorities", "cellSpursSetPriority", "cellSpursRequestIdleSpu", "cellSpursShutdownWorkload",
             "cellSpursWaitForWorkloadShutdown", "cellSpursRemoveWorkload", "cellSpursSendWorkloadSignal",
             "cellSpursGetWorkloadData", "cellSpursWorkloadAttributeSetShutdownCompletionEventHook",
             "cellSpursReadyCountAdd", "cellSpursReadyCountCompareAndSwap", "cellSpursReadyCountSwap",
             "_cellSpursWorkloadFlagReceiver2", "cellSpursAddUrgentCall", "cellSpursEnableExceptionEventHandler",
             "cellSpursUnsetGlobalExceptionEventHandler", "cellSpursTraceInitialize", "cellSpursTraceStart",
             "cellSpursTraceStop", "cellSpursTraceFinalize", "cellSpursCreateTaskset2",
             "_cellSpursTasksetAttribute2Initialize", "_cellSpursTaskAttribute2Initialize",
             "cellSpursTasksetAttributeEnableClearLS", "cellSpursTasksetSetExceptionEventHandler",
             "cellSpursTasksetUnsetExceptionEventHandler", "cellSpursTasksetGetSpursAddress",
             "cellSpursLookUpTasksetAddress", "cellSpursGetTasksetId", "cellSpursEventFlagClear",
             "cellSpursEventFlagTryWait", "cellSpursEventFlagGetDirection", "cellSpursEventFlagGetClearMode",
             "cellSpursEventFlagGetTasksetAddress", "cellSpursBarrierGetTasksetAddress", "_cellSpursLFQueueInitialize",
             "cellSpursCreateJobChain", "cellSpursKickJobChain", "cellSpursGetJobChainId", "cellSpursGetJobChainInfo",
             "cellSpursJobChainGetError", "cellSpursJobChainGetSpursAddress",
             "cellSpursJobChainAttributeSetJobTypeMemoryCheck", "cellSpursJobChainSetExceptionEventHandler",
             "cellSpursJobChainUnsetExceptionEventHandler", "cellSpursJobGuardInitialize", "cellSpursJobGuardNotify",
             "cellSpursJobGuardReset", "cellSpursJobSetMaxGrab"}) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("cellSpurs", recomp::nid_of(name));
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
        CHECK_FALSE(fn->internal);
    }
    const hle::Function* helper = hle::registry().find("cellSpurs", recomp::nid_of("_pspocket_spurs_event_helper"));
    REQUIRE(helper != nullptr);
    CHECK(helper->internal);
}

TEST_CASE("cellSpurs: cellSpursInitialize, the attribute's memory container, the information and thread IDs",
    "[hle][spurs][spurs-api]")
{
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();

    // cellSpursInitialize: revision and SDK version 0, exitIfNoWork's low byte only, as RPCS3 and libsre.
    CHECK(a.call("cellSpursInitialize", 0, 2, 200, 2016, 0) == code(CORE | kNull));
    CHECK(a.call("cellSpursInitialize", kSpurs + 0x40, 2, 200, 2016, 0) == code(CORE | kAlign));
    REQUIRE(a.call("cellSpursInitialize", kSpurs, 2, 200, 2016, 0x101) == 0);
    CHECK(ps_read32(b, kSpurs + 0xDA0) == 0);         // revision
    CHECK(ps_read32(b, kSpurs + 0xD80) == 1);         // flags: SAF_EXIT_IF_NO_WORK
    CHECK(ps_read8(b, kSpurs + 0x74) == 0x80);        // flags1: SF1_EXIT_IF_NO_WORK
    CHECK(ps_read8(b, kSpurs + 0xD64) == 1);          // handlerDirty: initialize ends with cellSpursWakeUp
    CHECK(ps_read32(b, kSpurs + 0xD68) == 1);         // enableEH, as initialize's cellSpursEnableExceptionEventHandler
    CHECK(ps_read32(b, kSpurs + 0xB0) == 0xFFFF);     // SPURS1: workloads 16..31 taken
    CHECK(a.call("cellSpursInitialize", kSpurs, 2, 200, 2016, 0) == code(CORE | kStat)); // already
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
    CHECK(a.call("cellSpursInitialize", kSpurs, 2, 200, 2016, 0x100) == 0); // 0x100: low byte 0
    CHECK(ps_read32(b, kSpurs + 0xD80) == 0);
    CHECK(ps_read8(b, kSpurs + 0xD64) == 0);
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);

    // cellSpursAttributeSetMemoryContainerForSpuThread: NULL, ALIGN, STAT for an exclusive group type.
    REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 2, 200, 2016, 0) == 0);
    CHECK(a.call("cellSpursAttributeSetMemoryContainerForSpuThread", 0, 7) == code(CORE | kNull));
    CHECK(a.call("cellSpursAttributeSetMemoryContainerForSpuThread", kSpursAttr + 4, 7) == code(CORE | kAlign));
    REQUIRE(a.call("cellSpursAttributeSetSpuThreadGroupType", kSpursAttr, 0x18) == 0);
    CHECK(a.call("cellSpursAttributeSetMemoryContainerForSpuThread", kSpursAttr, 7) == code(CORE | kStat));
    REQUIRE(a.call("cellSpursAttributeSetSpuThreadGroupType", kSpursAttr, 0) == 0);
    REQUIRE(a.call("cellSpursAttributeSetMemoryContainerForSpuThread", kSpursAttr, 0x7777) == 0);
    CHECK(ps_read32(b, kSpursAttr + 0x2C) == 0x7777);
    CHECK(ps_read32(b, kSpursAttr + 0x28) == 0x40000000u); // SAF_SPU_MEMORY_CONTAINER_SET
    CHECK(a.call("cellSpursAttributeSetSpuThreadGroupType", kSpursAttr, 0x18) == code(CORE | kStat));
    std::memcpy(b + kOut, "Pref", 4);
    REQUIRE(a.call("cellSpursAttributeSetNamePrefix", kSpursAttr, kOut, 4) == 0);
    REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0); // the container is not used natively

    // cellSpursGetInfo (libsre's fields).
    constexpr u32 kInfo = kData + 0x5000;
    std::memset(b + kInfo, 0xAB, 0x118);
    CHECK(a.call("cellSpursGetInfo", 0, kInfo) == code(CORE | kNull));
    CHECK(a.call("cellSpursGetInfo", kSpurs, 0) == code(CORE | kNull));
    CHECK(a.call("cellSpursGetInfo", kSpurs + 0x40, kInfo) == code(CORE | kAlign));
    REQUIRE(a.call("cellSpursGetInfo", kSpurs, kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 0x00) == 2);    // nSpus
    CHECK(ps_read32(b, kInfo + 0x04) == 200);  // SPU thread group priority
    CHECK(ps_read32(b, kInfo + 0x08) == 2016); // PPU priority
    CHECK(ps_read8(b, kInfo + 0x0C) == 0);     // exitIfNoWork
    CHECK(ps_read8(b, kInfo + 0x0D) == 0);     // spurs2
    CHECK(ps_read32(b, kInfo + 0x10) == 0);    // no trace buffer
    CHECK(ps_read64(b, kInfo + 0x18) == 0);
    CHECK(ps_read32(b, kInfo + 0x20) == 0);
    CHECK(ps_read32(b, kInfo + 0x24) == 0);    // no SPU thread group natively
    for (u32 i = 0; i < 8; i++) CHECK(ps_read32(b, kInfo + 0x28 + 4 * i) == 0);
    CHECK(ps_read64(b, kInfo + 0x48) == 0xFFFFFFFF); // handler threads: none
    CHECK(ps_read64(b, kInfo + 0x50) == 0xFFFFFFFF);
    CHECK(std::memcmp(b + kInfo + 0x58, "Pref", 5) == 0); // NUL-terminated
    CHECK(ps_read32(b, kInfo + 0x68) == 4);
    CHECK(ps_read32(b, kInfo + 0x6C) == 0);
    CHECK(ps_read32(b, kInfo + 0x70) == 0);
    CHECK(ps_read8(b, kInfo + 0x74) == 0xAB); // the padding is left

    // The SPU count, thread group and thread IDs.
    CHECK(a.call("cellSpursGetNumSpuThread", kSpurs, 0) == code(CORE | kNull));
    CHECK(a.call("cellSpursGetNumSpuThread", kSpurs + 0x40, kOut) == code(CORE | kAlign));
    REQUIRE(a.call("cellSpursGetNumSpuThread", kSpurs, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 2);
    CHECK(a.call("cellSpursGetSpuThreadGroupId", 0, kOut) == code(CORE | kNull));
    CHECK(a.call("cellSpursGetSpuThreadGroupId", kSpurs + 0x40, kOut) == code(CORE | kAlign));
    ps_write32(b, kOut, 0x1234);
    REQUIRE(a.call("cellSpursGetSpuThreadGroupId", kSpurs, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0);
    CHECK(a.call("cellSpursGetSpuThreadId", kSpurs, 0, kOut + 0x40) == code(CORE | kNull));
    CHECK(a.call("cellSpursGetSpuThreadId", kSpurs, kOut, 0) == code(CORE | kNull));
    std::memset(b + kOut, 0xEE, 0x20);
    ps_write32(b, kOut + 0x40, 5);
    REQUIRE(a.call("cellSpursGetSpuThreadId", kSpurs, kOut, kOut + 0x40) == 0);
    CHECK(ps_read32(b, kOut + 0x40) == 2); // min(5, nSpus)
    CHECK(ps_read32(b, kOut + 0) == 0);
    CHECK(ps_read32(b, kOut + 4) == 0);
    CHECK(ps_read32(b, kOut + 8) == 0xEEEEEEEE); // not written past the count
    ps_write32(b, kOut + 0x40, 1);
    REQUIRE(a.call("cellSpursGetSpuThreadId", kSpurs, kOut, kOut + 0x40) == 0);
    CHECK(ps_read32(b, kOut + 0x40) == 1);
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

TEST_CASE("cellSpurs: cellSpursGetSpuGuid decodes the .SpuGUID section's four ila instructions, as libsre",
    "[hle][spurs][spurs-api]")
{
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    constexpr u32 kGuid = kData + 0x100;
    // ila $2, (data << 2) | i for i = 0..3: GUID 0x0123456789ABCDEF.
    const u16 data[4] = {0x0123, 0x4567, 0x89AB, 0xCDEF};
    for (u32 i = 0; i < 4; i++) ps_write32(b, kGuid + 4 * i, 0x42000002u | (((u32{data[i]} << 2) | i) << 7));
    CHECK(a.call("cellSpursGetSpuGuid", 0, kOut) == code(CORE | kNull));
    CHECK(a.call("cellSpursGetSpuGuid", kGuid, 0) == code(CORE | kNull));
    CHECK(a.call("cellSpursGetSpuGuid", kGuid, kOut + 4) == code(CORE | kAlign));
    REQUIRE(a.call("cellSpursGetSpuGuid", kGuid, kOut) == 0);
    CHECK(ps_read64(b, kOut) == 0x0123456789ABCDEFull);

    // Not the section: another register, the indices out of order, another instruction.
    ps_write64(b, kOut, 0);
    ps_write32(b, kGuid, ps_read32(b, kGuid) | 1); // rt 3
    CHECK(a.call("cellSpursGetSpuGuid", kGuid, kOut) == code(CORE | kInval));
    ps_write32(b, kGuid, 0x42000002u | ((u32{data[0]} << 2 | 1) << 7)); // index 1 in the first word
    CHECK(a.call("cellSpursGetSpuGuid", kGuid, kOut) == code(CORE | kInval));
    ps_write32(b, kGuid, 0x40800002u); // il $2, 0
    CHECK(a.call("cellSpursGetSpuGuid", kGuid, kOut) == code(CORE | kInval));
    CHECK(ps_read64(b, kOut) == 0);
}

TEST_CASE("cellSpurs: workload priorities, contention, idle SPU requests, signals, data and ready counts",
    "[hle][spurs][spurs-api]")
{
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    init_spurs(a);
    const u32 wid = add_idle_workload(a, b, {0, 0, 0, 0, 0, 0, 0, 0}); // never on SPU 0: the worker leaves it alone
    REQUIRE(wid == 0);
    const u32 info = kSpurs + 0xB00 + 0x20 * wid;

    // cellSpursSetMaxContention: the core's checks; clamped to 8 in the low nibble.
    CHECK(a.call("cellSpursSetMaxContention", 0, wid, 3) == code(CORE | kNull));
    CHECK(a.call("cellSpursSetMaxContention", kSpurs + 0x40, wid, 3) == code(CORE | kAlign));
    CHECK(a.call("cellSpursSetMaxContention", kSpurs, 16, 3) == code(CORE | kInval));
    CHECK(a.call("cellSpursSetMaxContention", kSpurs, 5, 3) == code(CORE | kSrch));
    ps_write32(b, kSpurs + 0xD6C, 1);
    CHECK(a.call("cellSpursSetMaxContention", kSpurs, wid, 3) == code(CORE | kStat));
    ps_write32(b, kSpurs + 0xD6C, 0);
    ps_write8(b, kSpurs + 0x50 + wid, 0x71); // a high nibble the call keeps
    REQUIRE(a.call("cellSpursSetMaxContention", kSpurs, wid, 9) == 0);
    CHECK(ps_read8(b, kSpurs + 0x50 + wid) == 0x78);
    REQUIRE(a.call("cellSpursSetMaxContention", kSpurs, wid, 2) == 0);
    CHECK(ps_read8(b, kSpurs + 0x50 + wid) == 0x72);

    // cellSpursSetPriorities and cellSpursSetPriority.
    const u8 prio[8] = {0, 3, 0, 0, 0, 0, 0, 9};
    std::memcpy(b + kPrio, prio, 8);
    CHECK(a.call("cellSpursSetPriorities", 0, wid, kPrio) == code(CORE | kNull));
    CHECK(a.call("cellSpursSetPriorities", kSpurs, 16, kPrio) == code(CORE | kInval));
    CHECK(a.call("cellSpursSetPriorities", kSpurs, 7, kPrio) == code(CORE | kSrch));
    ps_write8(b, kPrio + 2, 16);
    CHECK(a.call("cellSpursSetPriorities", kSpurs, wid, kPrio) == code(CORE | kInval));
    ps_write8(b, kPrio + 2, 0);
    ps_write8(b, kSpurs + 0xBD, 0);
    REQUIRE(a.call("cellSpursSetPriorities", kSpurs, wid, kPrio) == 0);
    CHECK(std::memcmp(b + info + 0x18, prio, 8) == 0);
    CHECK(ps_read8(b, kSpurs + 0xBD) == 0xFF); // sysSrvMsgUpdateWorkload
    CHECK(a.call("cellSpursSetPriority", kSpurs, wid, 1, 16) == code(CORE | kInval));
    CHECK(a.call("cellSpursSetPriority", kSpurs, wid, 1, 2) == code(CORE | kInval)); // SPU 1 of a 1-SPU instance
    CHECK(a.call("cellSpursSetPriority", kSpurs, 9, 0, 2) == code(CORE | kSrch));
    ps_write8(b, kSpurs + 0xBD, 0);
    REQUIRE(a.call("cellSpursSetPriority", kSpurs, wid, 0, 0) == 0);
    CHECK(ps_read8(b, info + 0x18) == 0);
    CHECK(ps_read8(b, kSpurs + 0xBD) == 0x01); // SPU 0's bit

    // cellSpursRequestIdleSpu (libsre: the policy module's codes, a count of 8 accepted).
    CHECK(a.call("cellSpursRequestIdleSpu", 0, wid, 1) == code(PM | kNull));
    CHECK(a.call("cellSpursRequestIdleSpu", kSpurs, 16, 1) == code(PM | kInval));
    CHECK(a.call("cellSpursRequestIdleSpu", kSpurs, wid, 9) == code(PM | kInval));
    CHECK(a.call("cellSpursRequestIdleSpu", kSpurs, 3, 1) == code(PM | kSrch));
    REQUIRE(a.call("cellSpursRequestIdleSpu", kSpurs, wid, 8) == 0);
    CHECK(ps_read8(b, kSpurs + 0x10 + wid) == 8);
    REQUIRE(a.call("cellSpursRequestIdleSpu", kSpurs, wid, 0) == 0);

    // cellSpursSendWorkloadSignal: the workload's bit in wklSignal1.
    CHECK(a.call("cellSpursSendWorkloadSignal", 0, wid) == code(PM | kNull));
    CHECK(a.call("cellSpursSendWorkloadSignal", kSpurs, 16) == code(PM | kInval));
    CHECK(a.call("cellSpursSendWorkloadSignal", kSpurs, 4) == code(PM | kSrch));
    REQUIRE(a.call("cellSpursSendWorkloadSignal", kSpurs, wid) == 0);
    CHECK(ps_read16(b, kSpurs + 0x70) == 0x8000);

    // cellSpursGetWorkloadData.
    CHECK(a.call("cellSpursGetWorkloadData", kSpurs, 0, wid) == code(PM | kNull));
    CHECK(a.call("cellSpursGetWorkloadData", kSpurs, kOut, 16) == code(PM | kInval));
    CHECK(a.call("cellSpursGetWorkloadData", kSpurs, kOut, 2) == code(PM | kSrch));
    REQUIRE(a.call("cellSpursGetWorkloadData", kSpurs, kOut, wid) == 0);
    CHECK(ps_read64(b, kOut) == 0x5150);

    // The ready count: swap, compare and swap, add (clamped to 0..255), the value before each in *old.
    CHECK(a.call("cellSpursReadyCountSwap", kSpurs, wid, 0, 1) == code(PM | kNull));
    CHECK(a.call("cellSpursReadyCountSwap", kSpurs, wid, kOut, 256) == code(PM | kInval));
    CHECK(a.call("cellSpursReadyCountSwap", kSpurs, 6, kOut, 1) == code(PM | kSrch));
    REQUIRE(a.call("cellSpursReadyCountSwap", kSpurs, wid, kOut, 5) == 0);
    CHECK(ps_read32(b, kOut) == 0);
    CHECK(ps_read8(b, kSpurs + wid) == 5);
    REQUIRE(a.call("cellSpursReadyCountCompareAndSwap", kSpurs, wid, kOut, 4, 7) == 0); // fails: stays 5
    CHECK(ps_read32(b, kOut) == 5);
    CHECK(ps_read8(b, kSpurs + wid) == 5);
    REQUIRE(a.call("cellSpursReadyCountCompareAndSwap", kSpurs, wid, kOut, 5, 7) == 0);
    CHECK(ps_read32(b, kOut) == 5);
    CHECK(ps_read8(b, kSpurs + wid) == 7);
    CHECK(a.call("cellSpursReadyCountCompareAndSwap", kSpurs, wid, kOut, 256, 1) == code(PM | kInval));
    REQUIRE(a.call("cellSpursReadyCountAdd", kSpurs, wid, kOut, 250) == 0);
    CHECK(ps_read32(b, kOut) == 7);
    CHECK(ps_read8(b, kSpurs + wid) == 255);
    REQUIRE(a.call("cellSpursReadyCountAdd", kSpurs, wid, kOut, -300) == 0);
    CHECK(ps_read32(b, kOut) == 255);
    CHECK(ps_read8(b, kSpurs + wid) == 0);
    store8_release(b, kSpurs + 0x80 + wid, 1); // PREPARING: not runnable (the workers read the states atomically)
    CHECK(a.call("cellSpursReadyCountAdd", kSpurs, wid, kOut, 1) == code(PM | kStat));
    CHECK(a.call("cellSpursReadyCountStore", kSpurs, wid, 1) == code(PM | kStat));
    CHECK(a.call("cellSpursSendWorkloadSignal", kSpurs, wid) == code(PM | kStat));
    store8_release(b, kSpurs + 0x80 + wid, 2);
    // cellSpursReadyCountStore checks the value before the registration, as libsre (this order was SRCH before).
    CHECK(a.call("cellSpursReadyCountStore", kSpurs, 5, 256) == code(PM | kInval));
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

TEST_CASE("cellSpurs: the workload flag receivers; clearing for another workload is PERM", "[hle][spurs][spurs-api]")
{
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    init_spurs(a);
    const u32 w0 = add_idle_workload(a, b, {}), w1 = add_idle_workload(a, b, {});
    REQUIRE(w1 == w0 + 1);

    // The second receiver function sets and clears the receiver only; the flag is left.
    ps_write32(b, kSpurs + 0x6C, 0x12345678);
    REQUIRE(a.call("_cellSpursWorkloadFlagReceiver2", kSpurs, w1, 1, 0) == 0);
    CHECK(ps_read8(b, kSpurs + 0x77) == w1);
    CHECK(ps_read32(b, kSpurs + 0x6C) == 0x12345678);
    CHECK(a.call("_cellSpursWorkloadFlagReceiver2", kSpurs, w0, 1, 1) == code(PM | kBusy));
    CHECK(a.call("_cellSpursWorkloadFlagReceiver2", kSpurs, w0, 0, 0) == code(PM | kPerm));
    CHECK(a.call("_cellSpursWorkloadFlagReceiver2", kSpurs, 9, 1, 0) == code(PM | kSrch));
    CHECK(a.call("_cellSpursWorkloadFlagReceiver2", kSpurs, 16, 1, 0) == code(PM | kInval));
    REQUIRE(a.call("_cellSpursWorkloadFlagReceiver2", kSpurs, w1, 0, 0) == 0);
    CHECK(ps_read8(b, kSpurs + 0x77) == 0xFF);
    CHECK(ps_read32(b, kSpurs + 0x6C) == 0x12345678);

    // The first one resets the flag. Regression: clearing for a workload that is not the receiver reset the flag and
    // returned CELL_OK; RPCS3 and libsre return PERM and change nothing.
    REQUIRE(a.call("_cellSpursWorkloadFlagReceiver", kSpurs, w0, 1) == 0);
    CHECK(ps_read32(b, kSpurs + 0x6C) == 0xFFFFFFFF);
    ps_write32(b, kSpurs + 0x6C, 0x12345678);
    CHECK(a.call("_cellSpursWorkloadFlagReceiver", kSpurs, w1, 0) == code(PM | kPerm));
    CHECK(ps_read32(b, kSpurs + 0x6C) == 0x12345678);
    CHECK(ps_read8(b, kSpurs + 0x77) == w0);
    REQUIRE(a.call("_cellSpursWorkloadFlagReceiver", kSpurs, w0, 0) == 0);
    CHECK(ps_read8(b, kSpurs + 0x77) == 0xFF);
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

TEST_CASE("cellSpurs: shutdown, wait and remove; the shutdown-completion hook runs on the event helper",
    "[hle][spurs][spurs-api]")
{
    LinkedFixture f;
    InterpretMisses interpret;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    init_spurs(a);

    // The hook (spurs, wid, arg): stores its arguments and a done word at kHookOut.
    const u32 words[] = {
        0x3CC00000u | (kHookOut >> 16),    // lis r6, kHookOut@h
        0x60C60000u | (kHookOut & 0xFFFF), // ori r6, r6, kHookOut@l
        0x90660000u,                       // stw r3, 0(r6)
        0x90860004u,                       // stw r4, 4(r6)
        0x90A60008u,                       // stw r5, 8(r6)
        0x38E00001u,                       // li r7, 1
        0x90E6000Cu,                       // stw r7, 12(r6)
        0x4E800020u,                       // blr
    };
    for (u32 i = 0; i < std::size(words); i++) ps_write32(b, kHookCode + 4 * i, words[i]);
    ps_write32(b, kHookOpd, kHookCode);
    ps_write32(b, kHookOpd + 4, 0);
    std::memset(b + kHookOut, 0, 16);

    // A workload attribute with the hook (cellSpursWorkloadAttributeSetShutdownCompletionEventHook's checks first).
    ps_write32(b, kPm, as::bi(0));
    std::memset(b + kPrio, 0, 8);
    REQUIRE(a.call("_cellSpursWorkloadAttributeInitialize", kWattr, 1, 0x330000, kPm, 0x10, u64{0x99}, kPrio, 1, 1) == 0);
    CHECK(a.call("cellSpursWorkloadAttributeSetShutdownCompletionEventHook", 0, kHookOpd, 0x77) == code(PM | kNull));
    CHECK(a.call("cellSpursWorkloadAttributeSetShutdownCompletionEventHook", kWattr, 0, 0x77) == code(PM | kNull));
    CHECK(a.call("cellSpursWorkloadAttributeSetShutdownCompletionEventHook", kWattr + 4, kHookOpd, 0x77) ==
          code(PM | kAlign));
    REQUIRE(a.call("cellSpursWorkloadAttributeSetShutdownCompletionEventHook", kWattr, kHookOpd, 0x77) == 0);
    CHECK(ps_read32(b, kWattr + 0x30) == kHookOpd);
    CHECK(ps_read32(b, kWattr + 0x34) == 0x77);
    CHECK(ps_read64(b, kSpurs + 0xD28) == 0xFFFFFFFF); // no event helper yet
    REQUIRE(a.call("cellSpursAddWorkloadWithAttribute", kSpurs, kId, kWattr) == 0);
    const u32 wid = ps_read32(b, kId);
    CHECK((ps_read8(b, kSpurs + 0xA0 + wid) & 0x02) != 0); // a hook
    // The event helper now exists: its thread, the "_spuPrv" queue on a dynamic SPU port, the event port.
    const u32 helper = static_cast<u32>(ps_read64(b, kSpurs + 0xD28));
    REQUIRE(helper != 0xFFFFFFFF);
    CHECK(f.kernel->thread(helper) != nullptr);
    CHECK(f.kernel->event_queue(ps_read32(b, kSpurs + 0xD5C)) != nullptr);
    CHECK(ps_read32(b, kSpurs + 0xD60) != 0);
    CHECK(ps_read8(b, kSpurs + 0xC9) >= 0x10);

    // cellSpursShutdownWorkload's checks (libsre's order).
    CHECK(a.call("cellSpursShutdownWorkload", 0, wid) == code(PM | kNull));
    CHECK(a.call("cellSpursShutdownWorkload", kSpurs + 0x40, wid) == code(PM | kAlign));
    CHECK(a.call("cellSpursShutdownWorkload", kSpurs, 16) == code(PM | kInval));
    CHECK(a.call("cellSpursShutdownWorkload", kSpurs, 9) == code(PM | kSrch));
    CHECK(a.call("cellSpursWaitForWorkloadShutdown", kSpurs, 9) == code(PM | kSrch));
    CHECK(a.call("cellSpursRemoveWorkload", kSpurs, 9) == code(PM | kSrch));
    CHECK(a.call("cellSpursRemoveWorkload", kSpurs, wid) == code(PM | kStat)); // still runnable
    store8_release(b, kSpurs + 0x80 + wid, 3); // SHUTTING_DOWN
    CHECK(a.call("cellSpursRemoveWorkload", kSpurs, wid) == code(PM | kBusy));
    store8_release(b, kSpurs + 0x80 + wid, 2);

    // The shutdown: removable at once (no SPU holds it natively), the completion to the helper, which calls the hook;
    // the wait returns once it ran.
    REQUIRE(a.call("cellSpursShutdownWorkload", kSpurs, wid) == 0);
    CHECK(ps_read8(b, kSpurs + 0x80 + wid) == 4);
    REQUIRE(a.call("cellSpursWaitForWorkloadShutdown", kSpurs, wid) == 0);
    CHECK(ps_read32(b, kHookOut + 12) == 1);
    CHECK(ps_read32(b, kHookOut + 0) == kSpurs);
    CHECK(ps_read32(b, kHookOut + 4) == wid);
    CHECK(ps_read32(b, kHookOut + 8) == 0x77);
    CHECK((ps_read8(b, kSpurs + 0xA0 + wid) & 0x21) == 0x21); // completed, hook called
    CHECK(a.call("cellSpursShutdownWorkload", kSpurs, wid) == 0);  // again: nothing more
    CHECK(a.call("cellSpursWaitForWorkloadShutdown", kSpurs, wid) == 0);
    REQUIRE(a.call("cellSpursRemoveWorkload", kSpurs, wid) == 0);
    CHECK(ps_read8(b, kSpurs + 0x80 + wid) == 0);
    CHECK((ps_read32(b, kSpurs + 0xB0) & (0x80000000u >> wid)) == 0);
    CHECK((ps_read32(b, kSpurs + 0xB4) & (0x80000000u >> wid)) == 0);
    CHECK(a.call("cellSpursRemoveWorkload", kSpurs, wid) == code(PM | kSrch));

    // A workload without a hook: shut down and waited for, no hook (the helper has nothing to call).
    const u32 plain = add_idle_workload(a, b, {});
    REQUIRE(a.call("cellSpursShutdownWorkload", kSpurs, plain) == 0);
    REQUIRE(a.call("cellSpursWaitForWorkloadShutdown", kSpurs, plain) == 0);
    REQUIRE(a.call("cellSpursRemoveWorkload", kSpurs, plain) == 0);

    // cellSpursFinalize stops and joins the helper.
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
    CHECK(ps_read64(b, kSpurs + 0xD28) == 0xFFFFFFFF);
}

TEST_CASE("cellSpurs: without its internal function linked there is no event helper; a hook is only logged",
    "[hle][spurs][spurs-api]")
{
    Fixture f; // nothing linked
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    init_spurs(a);
    ps_write32(b, kPm, as::bi(0));
    std::memset(b + kPrio, 0, 8);
    REQUIRE(a.call("_cellSpursWorkloadAttributeInitialize", kWattr, 1, 0x330000, kPm, 0x10, u64{0}, kPrio, 1, 1) == 0);
    REQUIRE(a.call("cellSpursWorkloadAttributeSetShutdownCompletionEventHook", kWattr, kHookOpd, 0) == 0);
    REQUIRE(a.call("cellSpursAddWorkloadWithAttribute", kSpurs, kId, kWattr) == 0);
    CHECK(ps_read64(b, kSpurs + 0xD28) == 0xFFFFFFFF);
    const u32 wid = ps_read32(b, kId);
    REQUIRE(a.call("cellSpursShutdownWorkload", kSpurs, wid) == 0);
    REQUIRE(a.call("cellSpursWaitForWorkloadShutdown", kSpurs, wid) == 0); // does not wait for a hook nobody calls
    REQUIRE(a.call("cellSpursRemoveWorkload", kSpurs, wid) == 0);
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

TEST_CASE("cellSpurs: traces and the exception handlers' flags, as RPCS3", "[hle][spurs][spurs-api]")
{
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    init_spurs(a, 2);

    CHECK(a.call("cellSpursTraceStart", kSpurs) == code(CORE | kStat)); // no buffer
    CHECK(a.call("cellSpursTraceFinalize", kSpurs) == code(CORE | kStat));
    CHECK(a.call("cellSpursTraceInitialize", 0, kTrace, 0x1000, 0) == code(CORE | kNull));
    CHECK(a.call("cellSpursTraceInitialize", kSpurs, 0, 0x1000, 0) == code(CORE | kNull));
    CHECK(a.call("cellSpursTraceInitialize", kSpurs, kTrace + 8, 0x1000, 0) == code(CORE | kAlign));
    CHECK(a.call("cellSpursTraceInitialize", kSpurs, kTrace, 0x7F, 0) == code(CORE | kInval));
    CHECK(a.call("cellSpursTraceInitialize", kSpurs, kTrace, 0x1000, 4) == code(CORE | kInval));
    std::memset(b + kTrace, 0xEE, 0x80);
    REQUIRE(a.call("cellSpursTraceInitialize", kSpurs, kTrace, 0x1080, 3) == 0);
    CHECK(ps_read64(b, kSpurs + 0x900) == kTrace + 1); // the wrap flag in the low bit
    CHECK(ps_read32(b, kSpurs + 0x950) == 3);
    CHECK(ps_read64(b, kSpurs + 0x948) == 0x1000); // data size: past the header
    for (u32 i = 0; i < 6; i++) CHECK(ps_read32(b, kSpurs + 0x908 + 4 * i) == 8 + i * 0x80); // 0x80 packets per SPU
    for (u32 i = 0; i < 8; i++) {
        CHECK(ps_read32(b, kTrace + 4 * i) == 0);        // SPU threads: none natively
        CHECK(ps_read32(b, kTrace + 0x20 + 4 * i) == 0); // counts
    }
    CHECK(ps_read32(b, kTrace + 0x40) == 0); // thread group
    CHECK(ps_read32(b, kTrace + 0x44) == 2); // SPUs
    CHECK(ps_read8(b, kSpurs + 0x75) == 0);
    CHECK(a.call("cellSpursTraceInitialize", kSpurs, kTrace, 0x1080, 0) == code(CORE | kStat)); // a buffer already
    constexpr u32 kInfo = kData + 0x5000;
    REQUIRE(a.call("cellSpursGetInfo", kSpurs, kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 0x10) == kTrace);
    CHECK(ps_read64(b, kInfo + 0x18) == 0x1000);
    CHECK(ps_read32(b, kInfo + 0x20) == 1);
    REQUIRE(a.call("cellSpursTraceStart", kSpurs) == 0);
    CHECK(ps_read8(b, kSpurs + 0x75) == 1);
    REQUIRE(a.call("cellSpursTraceStop", kSpurs) == 0);
    CHECK(ps_read8(b, kSpurs + 0x75) == 2);
    CHECK(a.call("cellSpursTraceStop", kSpurs + 0x40) == code(CORE | kAlign));
    REQUIRE(a.call("cellSpursTraceFinalize", kSpurs) == 0);
    CHECK(ps_read64(b, kSpurs + 0x900) == 0);
    CHECK(ps_read32(b, kSpurs + 0x950) == 0);
    CHECK(ps_read8(b, kSpurs + 0x75) == 0);

    // cellSpursEnableExceptionEventHandler: the flag; cellSpursUnsetGlobalExceptionEventHandler: both words cleared.
    CHECK(a.call("cellSpursEnableExceptionEventHandler", 0, 1) == code(CORE | kNull));
    CHECK(a.call("cellSpursEnableExceptionEventHandler", kSpurs + 0x40, 1) == code(CORE | kAlign));
    REQUIRE(a.call("cellSpursEnableExceptionEventHandler", kSpurs, 0) == 0);
    CHECK(ps_read32(b, kSpurs + 0xD68) == 0);
    REQUIRE(a.call("cellSpursEnableExceptionEventHandler", kSpurs, 0x101) == 0); // a b8: the low byte
    CHECK(ps_read32(b, kSpurs + 0xD68) == 1);
    REQUIRE(a.call("cellSpursSetGlobalExceptionEventHandler", kSpurs, kHookOpd, 0x55) == 0);
    CHECK(a.call("cellSpursUnsetGlobalExceptionEventHandler", 0) == code(CORE | kNull));
    CHECK(a.call("cellSpursUnsetGlobalExceptionEventHandler", kSpurs + 0x40) == code(CORE | kAlign));
    ps_write32(b, kSpurs + 0xD6C, 1);
    CHECK(a.call("cellSpursUnsetGlobalExceptionEventHandler", kSpurs) == code(CORE | kStat));
    ps_write32(b, kSpurs + 0xD6C, 0);
    REQUIRE(a.call("cellSpursUnsetGlobalExceptionEventHandler", kSpurs) == 0);
    CHECK(ps_read64(b, kSpurs + 0xF80) == 0);
    CHECK(ps_read64(b, kSpurs + 0xF88) == 0);
    REQUIRE(a.call("cellSpursSetGlobalExceptionEventHandler", kSpurs, kHookOpd, 0x55) == 0); // free again
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

TEST_CASE("cellSpurs: CellSpursTaskset2 as libsre creates it; its attributes; tasks run in it; the taskset accessors",
    "[hle][spurs][spurs-api]")
{
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    init_spurs(a);

    // _cellSpursTasksetAttribute2Initialize: the defaults.
    std::memset(b + kTsAttr, 0xAB, 0x200);
    REQUIRE(a.call("_cellSpursTasksetAttribute2Initialize", kTsAttr, 7) == 0);
    CHECK(ps_read32(b, kTsAttr + 0x00) == 7);
    CHECK(ps_read32(b, kTsAttr + 0x04) == 0);
    CHECK(ps_read64(b, kTsAttr + 0x08) == 0);
    for (u32 i = 0; i < 8; i++) CHECK(ps_read8(b, kTsAttr + 0x10 + i) == 1);
    CHECK(ps_read32(b, kTsAttr + 0x18) == 8);
    CHECK(ps_read32(b, kTsAttr + 0x1C) == 0);
    CHECK(ps_read32(b, kTsAttr + 0x20) == 0);
    CHECK(ps_read32(b, kTsAttr + 0x1FC) == 0);
    CHECK(a.call("_cellSpursTasksetAttribute2Initialize", 0x70000000, 1) == code(TASK | kFault));

    // _cellSpursTaskAttribute2Initialize (libsre: all 256 bytes cleared).
    constexpr u32 kTaskAttr = kData + 0x5000;
    std::memset(b + kTaskAttr, 0xAB, 0x100);
    REQUIRE(a.call("_cellSpursTaskAttribute2Initialize", kTaskAttr, 3) == 0);
    CHECK(ps_read32(b, kTaskAttr) == 3);
    for (u32 i = 4; i < 0x100; i += 4) CHECK(ps_read32(b, kTaskAttr + i) == 0);

    // cellSpursTasksetAttributeEnableClearLS (on a CellSpursTasksetAttribute).
    constexpr u32 kTsAttr1 = kData + 0x5200;
    std::memset(b + kPrio, 1, 8);
    REQUIRE(a.call("_cellSpursTasksetAttributeInitialize", kTsAttr1, 1, 0x330000, 0, kPrio, 1) == 0);
    CHECK(a.call("cellSpursTasksetAttributeEnableClearLS", 0, 1) == code(TASK | kNull));
    CHECK(a.call("cellSpursTasksetAttributeEnableClearLS", kTsAttr1 + 4, 1) == code(TASK | kAlign));
    REQUIRE(a.call("cellSpursTasksetAttributeEnableClearLS", kTsAttr1, 5) == 0);
    CHECK(ps_read32(b, kTsAttr1 + 0x24) == 1);
    REQUIRE(a.call("cellSpursTasksetAttributeEnableClearLS", kTsAttr1, 0) == 0);
    CHECK(ps_read32(b, kTsAttr1 + 0x24) == 0);

    // cellSpursCreateTaskset2 without an attribute: the defaults (priority 1, maximum contention 8), 10496 bytes, the
    // two handlers' descriptors, two event flags.
    std::memset(b + kTs, 0xAB, 0x2900);
    REQUIRE(a.call("cellSpursCreateTaskset2", kSpurs, kTs, 0) == 0);
    const u32 wid = ps_read32(b, kTs + 0x74);
    REQUIRE(wid < 16);
    CHECK(ps_read64(b, kTs + 0x60) == kSpurs);
    CHECK(ps_read32(b, kTs + 0x1890) == 10496);
    CHECK(ps_read64(b, kTs + 0x1900) == (0x400u | 0xFu)); // the exit code handler (0x710 bytes: 15 lines)
    CHECK(ps_read64(b, kTs + 0x78) == (0x500u | 0xDu));   // the Task2 handler (0x670 bytes: 13 lines)
    CHECK(ps_read32(b, kTs + 0x28FC) == 0);               // cleared to its end
    const u32 info = kSpurs + 0xB00 + 0x20 * wid;
    for (u32 i = 0; i < 8; i++) CHECK(ps_read8(b, info + 0x18 + i) == 1);
    CHECK((ps_read8(b, kSpurs + 0x50 + wid) & 0x0F) == 8);
    REQUIRE(f.kernel->event_flag(ps_read32(b, kTs + 0x1898)) != nullptr);
    REQUIRE(f.kernel->event_flag(ps_read32(b, kTs + 0x189C)) != nullptr);
    CHECK(ps_read64(b, kTs + 0x18A8) == 0); // no task name buffer

    // A task created with cellSpursCreateTask runs in it.
    write_task_elf(b, kElf, marker_task());
    ps_write32(b, kArg, kTaskOut);
    ps_write32(b, kArg + 4, 0x7A5C2);
    std::memset(b + kTaskOut, 0, 16);
    REQUIRE(a.call("cellSpursCreateTask", kTs, kId, kElf, 0, 0, 0, kArg) == 0);
    REQUIRE(wait_for([&] { return (load64_acquire(b, kTaskOut) & 0xFFFFFFFF) == 0x7A5C2; })); // the PUT's head (D-585)

    // With an attribute: its name and a task name buffer, cleared; a misaligned buffer is ALIGN after the creation.
    REQUIRE(a.call("_cellSpursTasksetAttribute2Initialize", kTsAttr, 1) == 0);
    std::memcpy(b + kOut, "Two", 4);
    ps_write32(b, kTsAttr + 0x04, kOut);
    ps_write64(b, kTsAttr + 0x08, 0xABCD);
    ps_write32(b, kTsAttr + 0x18, 3);
    ps_write32(b, kTsAttr + 0x20, kNames);
    std::memset(b + kNames, 0xEE, 0x1000);
    constexpr u32 t2 = kData + 0x20000; // 10496 bytes
    REQUIRE(a.call("cellSpursCreateTaskset2", kSpurs, t2, kTsAttr) == 0);
    CHECK(ps_read64(b, t2 + 0x68) == 0xABCD);
    CHECK(ps_read64(b, t2 + 0x18A8) == kNames);
    CHECK(ps_read32(b, kNames) == 0);
    CHECK(ps_read32(b, kNames + 0xFFC) == 0);
    const u32 wid2 = ps_read32(b, t2 + 0x74);
    CHECK(ps_read64(b, kSpurs + 0xE00 + 0x10 * wid2 + 8) == kOut); // the name
    CHECK((ps_read8(b, kSpurs + 0x50 + wid2) & 0x0F) == 3);
    ps_write32(b, kTsAttr + 0x20, kNames + 8);
    CHECK(a.call("cellSpursCreateTaskset2", kSpurs, t2 + 0x2900 + 0x80, kTsAttr) == code(TASK | kAlign));
    CHECK(a.call("cellSpursCreateTaskset2", 0, t2, kTsAttr) == code(TASK | kNull));

    // The exception handler (libsre: workload IDs up to 31 are accepted).
    CHECK(a.call("cellSpursTasksetSetExceptionEventHandler", 0, kHookOpd, 0) == code(TASK | kNull));
    CHECK(a.call("cellSpursTasksetSetExceptionEventHandler", kTs, 0, 0) == code(TASK | kNull));
    CHECK(a.call("cellSpursTasksetSetExceptionEventHandler", kTs + 0x40, kHookOpd, 0) == code(TASK | kAlign));
    REQUIRE(a.call("cellSpursTasksetSetExceptionEventHandler", kTs, kHookOpd, 0x1234) == 0);
    CHECK(ps_read64(b, kTs + 0x1880) == kHookOpd);
    CHECK(ps_read64(b, kTs + 0x1888) == 0x1234);
    CHECK(a.call("cellSpursTasksetSetExceptionEventHandler", kTs, kHookOpd, 0) == code(TASK | kBusy));
    REQUIRE(a.call("cellSpursTasksetUnsetExceptionEventHandler", kTs) == 0);
    CHECK(ps_read64(b, kTs + 0x1880) == 0);
    CHECK(ps_read64(b, kTs + 0x1888) == 0);
    ps_write32(b, kTs + 0x74, 32);
    CHECK(a.call("cellSpursTasksetSetExceptionEventHandler", kTs, kHookOpd, 0) == code(TASK | kInval));
    CHECK(a.call("cellSpursTasksetUnsetExceptionEventHandler", kTs) == code(TASK | kInval));
    CHECK(a.call("cellSpursTasksetGetSpursAddress", kTs, kOut) == code(TASK | kInval));
    REQUIRE(a.call("cellSpursGetTasksetId", kTs, kOut) == 0); // not checked
    CHECK(ps_read32(b, kOut) == 32);
    ps_write32(b, kTs + 0x74, 20); // a SPURS2 workload ID: valid for libsre
    REQUIRE(a.call("cellSpursTasksetUnsetExceptionEventHandler", kTs) == 0);
    ps_write32(b, kTs + 0x74, wid);

    // The address getters and the look-up by workload ID.
    CHECK(a.call("cellSpursTasksetGetSpursAddress", kTs, 0) == code(TASK | kNull));
    CHECK(a.call("cellSpursTasksetGetSpursAddress", kTs + 0x40, kOut) == code(TASK | kAlign));
    REQUIRE(a.call("cellSpursTasksetGetSpursAddress", kTs, kOut) == 0);
    CHECK(ps_read32(b, kOut) == kSpurs);
    CHECK(a.call("cellSpursGetTasksetId", 0, kOut) == code(TASK | kNull));
    CHECK(a.call("cellSpursGetTasksetId", kTs, 0) == code(TASK | kNull));
    REQUIRE(a.call("cellSpursGetTasksetId", kTs, kOut) == 0);
    CHECK(ps_read32(b, kOut) == wid);
    CHECK(a.call("cellSpursLookUpTasksetAddress", kSpurs, 0, wid) == code(TASK | kNull));
    CHECK(a.call("cellSpursLookUpTasksetAddress", kSpurs, kOut, 16) == code(TASK | kInval)); // PM INVAL ^ 0x100
    CHECK(a.call("cellSpursLookUpTasksetAddress", kSpurs, kOut, 12) == code(TASK | kSrch));
    CHECK(a.call("cellSpursLookUpTasksetAddress", 0, kOut, wid) == code(TASK | kNull));
    REQUIRE(a.call("cellSpursLookUpTasksetAddress", kSpurs, kOut, wid2) == 0);
    CHECK(ps_read32(b, kOut) == t2);
    REQUIRE(a.call("cellSpursShutdownTaskset", kTs) == 0);
    REQUIRE(a.call("cellSpursJoinTaskset", kTs) == 0);
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

TEST_CASE("cellSpurs: event flag getters, clear and try-wait; the barrier's taskset; the SPURS LF queue",
    "[hle][spurs][spurs-api]")
{
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    init_spurs(a);
    std::memset(b + kPrio, 0, 8);
    REQUIRE(a.call("cellSpursCreateTaskset", kSpurs, kTs2, 0, kPrio, 1) == 0);

    // An ANY2ANY flag of the taskset, cleared by hand.
    REQUIRE(a.call("_cellSpursEventFlagInitialize", 0, kTs2, kFlag, 1 /* MANUAL */, 3 /* ANY2ANY */) == 0);
    CHECK(a.call("cellSpursEventFlagGetDirection", 0, kOut) == code(TASK | kNull));
    CHECK(a.call("cellSpursEventFlagGetDirection", kFlag + 0x40, 0) == code(TASK | kAlign)); // libsre: ALIGN first
    CHECK(a.call("cellSpursEventFlagGetDirection", kFlag, 0) == code(TASK | kNull));
    REQUIRE(a.call("cellSpursEventFlagGetDirection", kFlag, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 3);
    REQUIRE(a.call("cellSpursEventFlagGetClearMode", kFlag, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 1);
    CHECK(a.call("cellSpursEventFlagGetClearMode", kFlag + 0x40, kOut) == code(TASK | kAlign));
    REQUIRE(a.call("cellSpursEventFlagGetTasksetAddress", kFlag, kOut) == 0);
    CHECK(ps_read32(b, kOut) == kTs2);
    CHECK(a.call("cellSpursEventFlagGetTasksetAddress", kFlag, 0) == code(TASK | kNull));

    REQUIRE(a.call("cellSpursEventFlagSet", kFlag, 0x00F0) == 0);
    CHECK(a.call("cellSpursEventFlagClear", 0, 1) == code(TASK | kNull));
    CHECK(a.call("cellSpursEventFlagClear", kFlag + 0x40, 1) == code(TASK | kAlign));
    REQUIRE(a.call("cellSpursEventFlagClear", kFlag, 0x0030) == 0);
    CHECK(ps_read16(b, kFlag) == 0x00C0);

    // cellSpursEventFlagTryWait: the condition met or BUSY; it needs no lv2 queue (no STAT).
    ps_write16(b, kOut, 0x0081);
    REQUIRE(a.call("cellSpursEventFlagTryWait", kFlag, kOut, 0 /* OR */) == 0);
    CHECK(ps_read16(b, kOut) == 0x0080);
    CHECK(ps_read16(b, kFlag) == 0x00C0); // MANUAL: not cleared
    ps_write16(b, kOut, 0x0081);
    CHECK(a.call("cellSpursEventFlagTryWait", kFlag, kOut, 1 /* AND */) == code(TASK | kBusy));
    CHECK(a.call("cellSpursEventFlagTryWait", kFlag, kOut, 2) == code(TASK | kInval));
    CHECK(a.call("cellSpursEventFlagTryWait", kFlag, 0, 0) == code(TASK | kNull));

    // A flag of the whole instance: no taskset.
    constexpr u32 kIwl = kFlag + 0x800;
    REQUIRE(a.call("_cellSpursEventFlagInitialize", kSpurs, 0, kIwl, 0, 1 /* SPU2PPU */) == 0);
    ps_write32(b, kOut, 0x1234);
    REQUIRE(a.call("cellSpursEventFlagGetTasksetAddress", kIwl, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0);
    REQUIRE(a.call("cellSpursEventFlagGetDirection", kIwl, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 1);

    // cellSpursBarrierGetTasksetAddress.
    REQUIRE(a.call("cellSpursBarrierInitialize", kTs2, kBarrier, 4) == 0);
    CHECK(a.call("cellSpursBarrierGetTasksetAddress", 0, kOut) == code(TASK | kNull));
    CHECK(a.call("cellSpursBarrierGetTasksetAddress", kBarrier + 0x40, 0) == code(TASK | kAlign));
    CHECK(a.call("cellSpursBarrierGetTasksetAddress", kBarrier, 0) == code(TASK | kNull));
    REQUIRE(a.call("cellSpursBarrierGetTasksetAddress", kBarrier, kOut) == 0);
    CHECK(ps_read32(b, kOut) == kTs2);

    // _cellSpursLFQueueInitialize: a taskset's queue through cellSyncLFQueueInitialize, its errors as task errors.
    REQUIRE(a.call("_cellSpursLFQueueInitialize", kTs2, kLfq, kLfqBuf, 16, 8, 2 /* PPU2SPU */) == 0);
    CHECK(ps_read32(b, kLfq + 0x10) == 16);    // size
    CHECK(ps_read32(b, kLfq + 0x14) == 8);     // depth
    CHECK(ps_read64(b, kLfq + 0x18) == kLfqBuf);
    CHECK(ps_read64(b, kLfq + 0x70) == kTs2);  // the signal argument: the taskset
    CHECK(ps_read32(b, kLfq + 0x2C) == 2);     // set up
    CHECK(a.call("_cellSpursLFQueueInitialize", kTs2, kLfq + 0x80, kLfqBuf, 16, 0, 2) == code(TASK | kInval));
    CHECK(a.call("_cellSpursLFQueueInitialize", kTs2, kLfq + 0x80, kLfqBuf + 8, 16, 8, 2) == code(TASK | kAlign));
    CHECK(a.call("_cellSpursLFQueueInitialize", kTs2, 0, kLfqBuf, 16, 8, 2) == code(TASK | kNull));
    ps_write32(b, kTs2 + 0x74, 32);
    CHECK(a.call("_cellSpursLFQueueInitialize", kTs2, kLfq + 0x80, kLfqBuf, 16, 8, 2) == code(TASK | kInval));
    ps_write32(b, kTs2 + 0x74, 0);
    REQUIRE(a.call("_cellSpursLFQueueInitialize", kTs2 | 1, kLfq + 0x80, kLfqBuf, 16, 8, 1) == 0); // not 16-aligned
    CHECK(ps_read64(b, kLfq + 0x80 + 0x70) == (kTs2 | 1));
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

TEST_CASE("cellSpurs: SPURS2 (cellSpursInitializeWithAttribute2): 32 workloads; kernel 2 runs workloads 16..31",
    "[hle][spurs][spurs-api]")
{
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 2, 200, 2016, 0) == 0);
    CHECK(a.call("cellSpursInitializeWithAttribute2", kSpurs, 0) == code(CORE | kNull));
    CHECK(a.call("cellSpursInitializeWithAttribute2", kSpurs, kSpursAttr + 4) == code(CORE | kAlign));
    ps_write32(b, kSpursAttr, 3);
    CHECK(a.call("cellSpursInitializeWithAttribute2", kSpurs, kSpursAttr) == code(CORE | kInval));
    ps_write32(b, kSpursAttr, 2);
    std::memset(b + kSpurs, 0xAB, 0x2000);
    REQUIRE(a.call("cellSpursInitializeWithAttribute2", kSpurs, kSpursAttr) == 0);
    CHECK(ps_read8(b, kSpurs + 0x74) == 0x40);        // SF1_32_WORKLOADS
    CHECK(ps_read32(b, kSpurs + 0xD80) == 4);         // SAF_SECOND_VERSION
    CHECK(ps_read32(b, kSpurs + 0xB0) == 0);          // all 32 workloads free
    CHECK(ps_read32(b, kSpurs + 0xD74) == 0x848);     // kernel 2's entry
    CHECK(ps_read32(b, kSpurs + 0x1FFC) == 0);        // 8 KB cleared
    constexpr u32 kInfo = kData + 0x5000;
    REQUIRE(a.call("cellSpursGetInfo", kSpurs, kInfo) == 0);
    CHECK(ps_read8(b, kInfo + 0x0D) == 1);
    CHECK(a.call("cellSpursRequestIdleSpu", kSpurs, 0, 1) == code(PM | kStat)); // the old API

    // Workloads 0..15: custom ones that never run (priority 0 everywhere).
    for (u32 i = 0; i < 16; i++) REQUIRE(add_idle_workload(a, b, {}, 3) == i);
    CHECK(ps_read8(b, kSpurs + 0x40) == 0); // SPURS2 keeps no minimum contention
    CHECK(ps_read8(b, kSpurs + 0x50) == 0x03);

    // Workload 16: a taskset (priority 1 on both SPUs, at most 2 SPUs): the second tables and the high nibbles.
    const u8 prio[8] = {1, 1, 0, 0, 0, 0, 0, 0};
    std::memcpy(b + kPrio, prio, 8);
    REQUIRE(a.call("cellSpursCreateTaskset", kSpurs, kTs2, 0x42, kPrio, 2) == 0);
    REQUIRE(ps_read32(b, kTs2 + 0x74) == 16);
    CHECK(ps_read64(b, kSpurs + 0x1000 + 0x00) == 0x200); // wklInfo2[0]: the taskset policy marker
    CHECK(ps_read64(b, kSpurs + 0x1000 + 0x08) == kTs2);
    CHECK(std::memcmp(b + kSpurs + 0x1000 + 0x18, prio, 8) == 0);
    CHECK(ps_read8(b, kSpurs + 0xD0) == 2);               // wklState2[0]: runnable
    CHECK(ps_read8(b, kSpurs + 0x50) == 0x23);            // max contention: 2 in the high nibble, 3 kept
    CHECK((ps_read32(b, kSpurs + 0xB0) & 0x8000) != 0);   // wklEnabled bit of workload 16

    // A task in it runs under kernel 2's selection, and the contention nibble is given back.
    write_task_elf(b, kElf, marker_task());
    ps_write32(b, kArg, kTaskOut);
    ps_write32(b, kArg + 4, 0x5B2);
    REQUIRE(a.call("cellSpursCreateTask", kTs2, kId, kElf, 0, 0, 0, kArg) == 0);
    REQUIRE(wait_for([&] { return (load64_acquire(b, kTaskOut) & 0xFFFFFFFF) == 0x5B2; }));
    REQUIRE(wait_for([&] { return (line_byte(b, kSpurs + 0x20) & 0xF0) == 0 && line_word(b, kTs2 + 0x30) == 0; }));

    // The PPU-side functions on workload 16.
    REQUIRE(a.call("cellSpursGetWorkloadData", kSpurs, kOut, 16) == 0);
    CHECK(ps_read64(b, kOut) == kTs2);
    REQUIRE(a.call("cellSpursSetMaxContention", kSpurs, 16, 1) == 0);
    CHECK(ps_read8(b, kSpurs + 0x50) == 0x13);
    REQUIRE(a.call("cellSpursSetPriority", kSpurs, 16, 1, 4) == 0);
    CHECK(ps_read8(b, kSpurs + 0x1000 + 0x18 + 1) == 4);
    CHECK(a.call("cellSpursSetMaxContention", kSpurs, 32, 1) == code(CORE | kInval));

    // Workload 17: a policy module that records what kernel 2 entered it with and leaves through [0x1E0].
    Program p;
    p.emit(as::rotqbyi(80, 4, 4)); // the record's EA: the argument's low word
    p.emit(as::il(73, kBuf + 0x10));
    p.dma16(PS_MFC_GET, 73, 80);
    p.emit(as::lqd(74, 0, 73));
    const usize skip = p.code.size();
    p.emit(0); // brnz $74: recorded already
    p.emit(as::ori(81, 0, 0)); // $0 before the puts
    p.put(5, 80, 0);           // the poll status
    p.put(81, 80, 16);         // $0: kernel 2's return address
    p.emit(as::lqa(7, 0x1E0));
    p.put(7, 80, 32);          // [0x1E0] exit, [0x1E4] select
    p.emit(as::lqa(8, 0x1D0));
    p.put(8, 80, 48);          // wklCurrentAddr, unique ID, wklCurrentId
    p.code[skip] = as::brnz(74, static_cast<s32>(p.code.size() - skip) * 4);
    p.emit(as::lqa(2, 0x1E0));
    p.emit(as::bisl(0, 2));
    constexpr u32 kModule = kPm + 0x100;
    for (usize i = 0; i < p.code.size(); i++) ps_write32(b, kModule + 4 * static_cast<u32>(i), p.code[i]);
    REQUIRE(a.call("cellSpursAddWorkload", kSpurs, kId, kModule, static_cast<u32>(p.code.size() * 4), u64{kRecord}, kPrio,
                1, 1) == 0);
    REQUIRE(ps_read32(b, kId) == 17);
    std::memset(b + kRecord, 0, 64);
    // Work arrives as its ready count (wklIdleSpuCountOrReadyCount2[1] in SPURS2).
    REQUIRE(a.call("cellSpursReadyCountStore", kSpurs, 17, 1) == 0);
    CHECK(ps_read8(b, kSpurs + 0x11) == 1);
    // The record's last PUT, polled at its first doubleword (the MFC stores it last, with release order, D-585).
    REQUIRE(wait_for([&] { return load64_acquire(b, kRecord + 48) == kModule; }));
    REQUIRE(a.call("cellSpursReadyCountStore", kSpurs, 17, 0) == 0);
    CHECK(ps_read32(b, kRecord + 48 + 12) == 17); // wklCurrentId
    CHECK(ps_read32(b, kRecord + 0) == 1);         // poll status: ready count
    CHECK(ps_read32(b, kRecord + 16) == 0x838);    // $0
    CHECK(ps_read32(b, kRecord + 32) == 0x838);    // [0x1E0]
    CHECK(ps_read32(b, kRecord + 36) == 0x290);    // [0x1E4]
    CHECK(ps_read64(b, kRecord + 48) == kModule);  // wklCurrentAddr

    // Its signal: wklSignal2's bit 0x4000.
    REQUIRE(wait_for([&] { return (line_byte(b, kSpurs + 0x21) & 0xF0) == 0; }));
    store8_release(b, kSpurs + 0x1020 + 0x18, 0); // off SPU 0 (as cellSpursSetPriority stores it)
    store8_release(b, kSpurs + 0x1020 + 0x19, 0); // and SPU 1: the signal stays
    REQUIRE(a.call("cellSpursSendWorkloadSignal", kSpurs, 17) == 0);
    CHECK(ps_read16(b, kSpurs + 0x78) == 0x4000);
    CHECK(ps_read16(b, kSpurs + 0x70) == 0);

    // Shut down, joined and removed: the taskset's workload 16 goes, its slot free for the next workload.
    REQUIRE(a.call("cellSpursShutdownTaskset", kTs2) == 0);
    CHECK(ps_read8(b, kSpurs + 0xD0) == 4);
    REQUIRE(a.call("cellSpursJoinTaskset", kTs2) == 0);
    CHECK(ps_read8(b, kSpurs + 0xD0) == 0);
    CHECK((ps_read32(b, kSpurs + 0xB0) & 0x8000) == 0);
    REQUIRE(a.call("cellSpursCreateTaskset", kSpurs, kTs2, 0x43, kPrio, 2) == 0);
    CHECK(ps_read32(b, kTs2 + 0x74) == 16);
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}
