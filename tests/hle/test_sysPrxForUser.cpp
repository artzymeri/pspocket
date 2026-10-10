// HLE functions on real lv2 threads: sysPrxForUser lwmutex (RPCS3 sys_lwmutex_.cpp semantics, including contention
// through the kernel's sleep queue), sys_ppu_thread_get_id and sys_time_get_system_time, cellSysmodule's load
// bookkeeping, lv2's null_func syscall 988, and liblv2's sysPs2Disc, sysBdMediaId and sysConsoleId wrappers.

#include <catch_amalgamated.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

#include <unistd.h>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/ppu/runtime.h"

#include "hle_test_util.hpp" // load32_acquire, line_half
#include "thread_check.hpp"

using namespace pspocket;

namespace {

constexpr u32 kData = 0x00100000;   // scratch guest data
constexpr u32 kStacks = 0x00200000; // one 64 KB guest stack per test thread

u64 r3_of(lv2::CellError e) { return lv2::to_r3(lv2::err(e)); }

struct Fixture {
    GuestMemory mem;
    std::unique_ptr<lv2::Kernel> kernel;
    std::unique_ptr<hle::HleProcess> hle;

    explicit Fixture(hle::ProcessConfig config = {})
    {
        config.spurs_workers = false; // these tests check the SPURS PPU API alone (test_spurs_run.cpp runs tasks)
        REQUIRE(mem.reserve());
        kernel = std::make_unique<lv2::Kernel>(mem);
        kernel->install();
        hle = std::make_unique<hle::HleProcess>(*kernel, 0x10000, 0x10000000, std::move(config));
        hle->install();
        REQUIRE(hle->main_memory().reserve(kData, 0x200000));
        REQUIRE(mem.commit(kData, 0x200000));
    }
    ~Fixture()
    {
        hle.reset();
        kernel.reset();
        pspocket::test::report_thread_checks(); // what failed on the tests' own threads (D-585)
    }
    u8* base() { return mem.base(); }
};

// A host thread with an lv2 identity (attach_thread) that calls HLE functions as guest code would.
struct GuestThread {
    Fixture& f;
    PPUContext ctx;
    lv2::PpuThread* t;

    GuestThread(Fixture& fx, u32 index, const char* name) : f(fx)
    {
        ps_ppu_context_init(&ctx);
        ctx.gpr[1] = kStacks + 0x10000 * (index + 1) - 0x100;
        t = f.kernel->attach_thread(ctx, name);
    }
    ~GuestThread() { f.kernel->detach_thread(t); }

    template <class... A>
    u64 call(std::string_view name, A... args)
    {
        const hle::Function* fn = nullptr;
        for (const hle::Function& x : hle::registry().functions())
            if (x.name == name) fn = &x;
        THREAD_REQUIRE(fn != nullptr); // called on the tests' own threads too (D-585)
        const u64 v[] = {0, static_cast<u64>(args)...}; // leading 0: no zero-length array for calls without arguments
        for (usize i = 0; i < sizeof...(A); i++) {
            if (i < 8) ctx.gpr[3 + i] = v[i + 1];
            else ps_write64(f.base(), static_cast<u32>(ctx.gpr[1]) + 0x70 + 8 * static_cast<u32>(i - 8), v[i + 1]);
        }
        const u64 sp = ctx.gpr[1];
        fn->fn(ctx, f.base());
        THREAD_CHECK(ctx.gpr[1] == sp); // stack variables released
        return ctx.gpr[3];
    }
};

constexpr u32 kAttr = kData, kLw = kData + 0x100;

void write_attr(u8* base, u32 protocol, u32 recursive)
{
    ps_write32(base, kAttr + 0, protocol);
    ps_write32(base, kAttr + 4, recursive);
    std::memcpy(base + kAttr + 8, "testlwm", 8);
}

// Read while other threads lock and unlock it (atomically): as a PPU reads them, with acquire order (D-585).
u32 owner(u8* base) { return hle::test::load32_acquire(base, kLw); }
u32 waiters(u8* base) { return hle::test::load32_acquire(base, kLw + 4); }

} // namespace

TEST_CASE("sys_lwmutex: create, recursive lock, trylock, unlock, destroy", "[hle][lwmutex]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    write_attr(base, 2 /* PRIORITY */, 0x10 /* RECURSIVE */);

    REQUIRE(a.call("sys_lwmutex_create", kLw, kAttr) == 0);
    CHECK(owner(base) == 0xFFFFFFFFu); // free
    CHECK(waiters(base) == 0);
    CHECK(ps_read32(base, kLw + 8) == 0x12); // recursive | protocol
    CHECK(ps_read32(base, kLw + 0x10) != 0);  // sleep queue ID

    REQUIRE(a.call("sys_lwmutex_lock", kLw, 0) == 0);
    CHECK(owner(base) == a.t->id);
    REQUIRE(a.call("sys_lwmutex_lock", kLw, 0) == 0);
    REQUIRE(a.call("sys_lwmutex_trylock", kLw) == 0);
    CHECK(ps_read32(base, kLw + 0xC) == 2); // recursion count
    REQUIRE(a.call("sys_lwmutex_unlock", kLw) == 0);
    REQUIRE(a.call("sys_lwmutex_unlock", kLw) == 0);
    CHECK(owner(base) == a.t->id);
    REQUIRE(a.call("sys_lwmutex_unlock", kLw) == 0);
    CHECK(owner(base) == 0xFFFFFFFFu);
    CHECK(a.call("sys_lwmutex_unlock", kLw) == r3_of(lv2::CELL_EPERM));

    REQUIRE(a.call("sys_lwmutex_destroy", kLw) == 0);
    CHECK(owner(base) == 0xFFFFFFFEu); // dead
    CHECK(a.call("sys_lwmutex_lock", kLw, 0) == r3_of(lv2::CELL_EINVAL));
}

TEST_CASE("sys_lwmutex: non-recursive relock, invalid attributes, destroy while owned", "[hle][lwmutex]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");

    write_attr(base, 2, 0x30);
    CHECK(a.call("sys_lwmutex_create", kLw, kAttr) == r3_of(lv2::CELL_EINVAL));
    write_attr(base, 3 /* PRIORITY_INHERIT: not accepted by sysPrxForUser */, 0x20);
    CHECK(a.call("sys_lwmutex_create", kLw, kAttr) == r3_of(lv2::CELL_EINVAL));

    write_attr(base, 1 /* FIFO */, 0x20 /* NOT_RECURSIVE */);
    REQUIRE(a.call("sys_lwmutex_create", kLw, kAttr) == 0);
    REQUIRE(a.call("sys_lwmutex_lock", kLw, 0) == 0);
    CHECK(a.call("sys_lwmutex_lock", kLw, 0) == r3_of(lv2::CELL_EDEADLK));
    CHECK(a.call("sys_lwmutex_trylock", kLw) == r3_of(lv2::CELL_EDEADLK));
    CHECK(a.call("sys_lwmutex_destroy", kLw) == r3_of(lv2::CELL_EBUSY));
    REQUIRE(a.call("sys_lwmutex_unlock", kLw) == 0);
    REQUIRE(a.call("sys_lwmutex_destroy", kLw) == 0);
}

TEST_CASE("sys_lwmutex: a contended lock sleeps in the kernel and is handed over on unlock", "[hle][lwmutex]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    write_attr(base, 2, 0x20);
    REQUIRE(a.call("sys_lwmutex_create", kLw, kAttr) == 0);
    REQUIRE(a.call("sys_lwmutex_lock", kLw, 0) == 0);

    std::atomic<u32> b_id{0};
    std::atomic<u64> b_trylock{0}, b_lock{~0ull}, b_unlock{~0ull};
    std::atomic<u32> owner_seen_by_b{0};
    std::thread tb([&] {
        GuestThread b(f, 1, "b");
        b_id = b.t->id;
        b_trylock = b.call("sys_lwmutex_trylock", kLw);
        b_lock = b.call("sys_lwmutex_lock", kLw, 0); // blocks until a unlocks
        owner_seen_by_b = owner(base);
        b_unlock = b.call("sys_lwmutex_unlock", kLw);
    });

    // Wait until b is asleep in the kernel (it registered as a waiter).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (waiters(base) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(waiters(base) == 1);
    CHECK(b_lock.load() == ~0ull); // still blocked
    CHECK(b_trylock.load() == r3_of(lv2::CELL_EBUSY));

    REQUIRE(a.call("sys_lwmutex_unlock", kLw) == 0);
    tb.join();
    CHECK(b_lock.load() == 0);
    CHECK(owner_seen_by_b.load() == b_id.load()); // handed over through the reserved state
    CHECK(b_unlock.load() == 0);
    CHECK(owner(base) == 0xFFFFFFFFu);
    CHECK(waiters(base) == 0);
    REQUIRE(a.call("sys_lwmutex_destroy", kLw) == 0);
}

TEST_CASE("sys_ppu_thread_get_id writes the caller's thread ID", "[hle]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    ps_write64(f.base(), kData, 0);
    REQUIRE(a.call("sys_ppu_thread_get_id", kData) == 0);
    CHECK(ps_read64(f.base(), kData) == a.t->id);
}

TEST_CASE("syscall 988 (debug PPU exception handler) returns CELL_OK like RPCS3's null_func", "[lv2]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    a.ctx.gpr[11] = 988;
    a.ctx.gpr[3] = 4;
    ps_ppu_syscall(&a.ctx, f.base());
    CHECK(a.ctx.gpr[3] == 0);
    CHECK(f.kernel->unimplemented_count() == 0);
}

TEST_CASE("sys_time_get_system_time: microseconds, increasing", "[hle]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    const u64 t0 = a.call("sys_time_get_system_time");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const u64 t1 = a.call("sys_time_get_system_time");
    CHECK(t1 > t0);
    CHECK(t1 - t0 >= 2000);
}

TEST_CASE("cellSysmodule: load counts, dependencies, default and virtual modules", "[hle][sysmodule]")
{
    Fixture f; // SDK 0x00360001 by default: the six external default modules are loaded
    GuestThread a(f, 0, "a");
    constexpr u64 kUnloaded = 0xFFFFFFFF80012003ull, kUnknown = 0xFFFFFFFF80012002ull;
    constexpr u32 kNet = 0x00, kHttps = 0x04, kRtc = 0x09, kFs = 0x0E, kNetctl = 0x14, kSysutil = 0x15;

    CHECK(a.call("cellSysmoduleInitialize") == 0);
    CHECK(a.call("cellSysmoduleIsLoaded", kSysutil) == 0);
    CHECK(a.call("cellSysmoduleIsLoaded", kFs) == 0);
    CHECK(a.call("cellSysmoduleIsLoaded", kNet) == kUnloaded);

    // NET depends on NETCTL; both load, both unload.
    REQUIRE(a.call("cellSysmoduleLoadModule", kNet) == 0);
    CHECK(a.call("cellSysmoduleIsLoaded", kNet) == 0);
    CHECK(a.call("cellSysmoduleIsLoaded", kNetctl) == 0);
    REQUIRE(a.call("cellSysmoduleUnloadModule", kNet) == 0);
    CHECK(a.call("cellSysmoduleIsLoaded", kNet) == kUnloaded);
    CHECK(a.call("cellSysmoduleIsLoaded", kNetctl) == kUnloaded);

    // Reference counted.
    REQUIRE(a.call("cellSysmoduleLoadModule", kRtc) == 0);
    REQUIRE(a.call("cellSysmoduleLoadModule", kRtc) == 0);
    REQUIRE(a.call("cellSysmoduleUnloadModule", kRtc) == 0);
    CHECK(a.call("cellSysmoduleIsLoaded", kRtc) == 0);
    REQUIRE(a.call("cellSysmoduleUnloadModule", kRtc) == 0);
    CHECK(a.call("cellSysmoduleIsLoaded", kRtc) == kUnloaded);

    // A default module loaded only by lv2 stays loaded.
    REQUIRE(a.call("cellSysmoduleUnloadModule", kSysutil) == 0);
    CHECK(a.call("cellSysmoduleIsLoaded", kSysutil) == 0);

    // HTTPS has no image of its own: loaded when its last dependency (HTTP) is.
    CHECK(a.call("cellSysmoduleIsLoaded", kHttps) == kUnloaded);
    REQUIRE(a.call("cellSysmoduleLoadModule", kHttps) == 0);
    CHECK(a.call("cellSysmoduleIsLoaded", kHttps) == 0);
    CHECK(a.call("cellSysmoduleIsLoaded", kNet) == 0); // SSL -> NET

    CHECK(a.call("cellSysmoduleLoadModule", 0x58) == kUnknown);
    CHECK(a.call("cellSysmoduleIsLoaded", 0xF000) == kUnknown); // internal, but not one the public calls take
    CHECK(a.call("cellSysmoduleFinalize") == 0);
}

TEST_CASE("cellSysmodule: internal module IDs, as RPCS3 has them (D-179)", "[hle][sysmodule]")
{
    Fixture f; // SDK 0x00360001: the NP trophy module (0xF035) is loaded by lv2
    GuestThread a(f, 0, "a");
    constexpr u64 kUnloaded = 0xFFFFFFFF80012003ull, kUnknown = 0xFFFFFFFF80012002ull;
    constexpr u32 kNpTrophy = 0xF035, kFs = 0x0E, kAdecAl = 0x2C;

    // CELL_SYSMODULE_SYSUTIL_NP_TROPHY: the public calls take it (Sniper: Ghost Warrior loads it at boot).
    CHECK(a.call("cellSysmoduleIsLoaded", kNpTrophy) == 0);
    CHECK(a.call("cellSysmoduleLoadModule", kNpTrophy) == 0);
    CHECK(a.call("cellSysmoduleUnloadModule", kNpTrophy) == 0);
    CHECK(a.call("cellSysmoduleIsLoaded", kNpTrophy) == 0); // still lv2's
    // One the public calls refuse, the internal ones take, with its dependencies (0xF018, libtiffdec: FS).
    CHECK(a.call("cellSysmoduleLoadModule", 0xF018) == kUnknown);
    REQUIRE(a.call("cellSysmoduleUnloadModule", kFs) == 0); // FS is a default: it stays loaded
    REQUIRE(a.call("cellSysmoduleLoadModuleInternal", 0xF018) == 0);
    CHECK(a.call("cellSysmoduleIsLoadedEx", 0xF018) == 0);
    REQUIRE(a.call("cellSysmoduleUnloadModuleInternal", 0xF018) == 0);
    CHECK(a.call("cellSysmoduleIsLoadedEx", 0xF018) == kUnloaded);
    // An internal dependency: 0xF054 (libatrac3multi) needs 0xF053 (libat3multidec), which needs ADEC_AL.
    REQUIRE(a.call("cellSysmoduleLoadModule", 0xF054) == 0);
    CHECK(a.call("cellSysmoduleIsLoadedEx", 0xF053) == 0);
    CHECK(a.call("cellSysmoduleIsLoaded", kAdecAl) == 0);
    REQUIRE(a.call("cellSysmoduleUnloadModule", 0xF054) == 0);
    CHECK(a.call("cellSysmoduleIsLoadedEx", 0xF053) == kUnloaded);
    CHECK(a.call("cellSysmoduleIsLoaded", kAdecAl) == kUnloaded);
    // The Ex calls: only LOADABLE_INTERNAL_MODULES_EX (0xF005 is, 0xF000 is not).
    CHECK(a.call("cellSysmoduleLoadModuleEx", 0xF000) == kUnknown);
    CHECK(a.call("cellSysmoduleLoadModuleEx", 0xF005) == 0);
    CHECK(a.call("cellSysmoduleUnloadModuleEx", 0xF005) == 0);
    // Out of the table, or a public ID given to the internal calls.
    CHECK(a.call("cellSysmoduleLoadModuleInternal", 0xF057) == kUnknown);
    CHECK(a.call("cellSysmoduleLoadModuleInternal", 0x0E) == kUnknown);
    CHECK(a.call("cellSysmoduleIsLoadedEx", 0x1000) == kUnknown);
    // The codecs whose PARAM.SFO flags lv2 leaves clear (VC-1, WMA, the ASF parser) are refused, as in RPCS3.
    CHECK(a.call("cellSysmoduleLoadModuleInternal", 0xF024) == kUnknown);
}

TEST_CASE("cellSysmoduleSetMemcontainer: a container with room, none, or an unusable one (RPCS3)", "[hle][sysmodule]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    constexpr u64 kInvalidContainer = 0xFFFFFFFF80012004ull;
    constexpr u64 kPageSize64K = 0x200; // SYS_MEMORY_PAGE_SIZE_64K
    constexpr u32 kId = kData + 0x40, kAddr = kData + 0x48;

    REQUIRE(lv2::sys_memory_container_create(*a.t, kId, 0x100000) == lv2::CELL_OK);
    const u32 ct = ps_read32(base, kId);
    CHECK(a.call("cellSysmoduleSetMemcontainer", ct) == 0); // GTA V's call: a container of its own, 1 MB free
    CHECK(a.call("cellSysmoduleSetMemcontainer", 0xFFFFFFFFu) == 0); // back to none
    CHECK(a.call("cellSysmoduleSetMemcontainer", ct + 1) == kInvalidContainer); // not a container
    // 64 KB left: less than a module load needs.
    REQUIRE(lv2::sys_memory_allocate_from_container(*a.t, 0xF0000, ct, kPageSize64K, kAddr) == lv2::CELL_OK);
    CHECK(a.call("cellSysmoduleSetMemcontainer", ct) == kInvalidContainer);
    CHECK(a.call("cellSysmoduleLoadModule", 0x09) == 0); // loads still work without a container
}

TEST_CASE("cellSpurs stage 1: attributes, native initialize, finalize", "[hle][spurs]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kAttrAddr = kData + 0x1000, kSpurs = kData + 0x2000, kPrefix = kData + 0x1800;
    constexpr u64 kInval = 0xFFFFFFFF80410702ull, kAlign = 0xFFFFFFFF80410710ull, kStat = 0xFFFFFFFF8041070Full,
                  kNull = 0xFFFFFFFF80410711ull, kLv2Inval = 0xFFFFFFFF80010002ull;

    std::memset(base + kAttrAddr, 0xCC, 0x200);
    REQUIRE(a.call("_cellSpursAttributeInitialize", kAttrAddr, 2, 0x330000, 5, 250, 1000, 1) == 0);
    CHECK(ps_read32(base, kAttrAddr + 0x0) == 2);
    CHECK(ps_read32(base, kAttrAddr + 0x4) == 0x330000);
    CHECK(ps_read32(base, kAttrAddr + 0x8) == 5);
    CHECK(ps_read32(base, kAttrAddr + 0xC) == 250);
    CHECK(ps_read32(base, kAttrAddr + 0x10) == 1000);
    CHECK(ps_read8(base, kAttrAddr + 0x14) == 1);
    CHECK(ps_read32(base, kAttrAddr + 0x28) == 0); // flags cleared with the rest
    CHECK(ps_read8(base, kAttrAddr + 0x1FF) == 0);
    CHECK(a.call("_cellSpursAttributeInitialize", kAttrAddr + 4, 2, 0, 5, 250, 1000, 0) == kAlign);
    CHECK(a.call("_cellSpursAttributeInitialize", 0, 2, 0, 5, 250, 1000, 0) == kNull);

    std::memcpy(base + kPrefix, "gta5spurs", 9);
    REQUIRE(a.call("cellSpursAttributeSetNamePrefix", kAttrAddr, kPrefix, 9) == 0);
    CHECK(std::memcmp(base + kAttrAddr + 0x15, "gta5spurs", 9) == 0);
    CHECK(ps_read32(base, kAttrAddr + 0x24) == 9);
    CHECK(a.call("cellSpursAttributeSetNamePrefix", kAttrAddr, kPrefix, 16) == kInval);

    REQUIRE(a.call("cellSpursAttributeSetSpuThreadGroupType", kAttrAddr, 0x18) == 0); // exclusive non-context
    CHECK(ps_read32(base, kAttrAddr + 0x28) == 0x20000000u);
    REQUIRE(a.call("cellSpursAttributeSetSpuThreadGroupType", kAttrAddr, 0) == 0);
    CHECK(ps_read32(base, kAttrAddr + 0x28) == 0);
    CHECK(a.call("cellSpursAttributeSetSpuThreadGroupType", kAttrAddr, 2) == kInval);

    std::memset(base + kSpurs, 0xAB, 0x1100);
    CHECK(a.call("cellSpursInitializeWithAttribute", kSpurs + 0x40, kAttrAddr) == kAlign);
    REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kAttrAddr) == 0);
    CHECK(ps_read32(base, kSpurs + 0xDA0) == 2);        // revision
    CHECK(ps_read32(base, kSpurs + 0xDA4) == 0x330000); // sdkVersion
    CHECK(ps_read8(base, kSpurs + 0x76) == 5);          // nSpus
    CHECK(ps_read32(base, kSpurs + 0xD84) == 250);      // spuPriority
    CHECK(ps_read32(base, kSpurs + 0xD88) == 1000);     // ppuPriority
    CHECK(ps_read32(base, kSpurs + 0xD80) == 1);        // flags: SAF_EXIT_IF_NO_WORK from exitIfNoWork
    CHECK(ps_read8(base, kSpurs + 0x74) == 0x80);       // flags1: SF1_EXIT_IF_NO_WORK
    CHECK(ps_read32(base, kSpurs + 0xB0) == 0xFFFF);    // wklEnabled
    CHECK(ps_read32(base, kSpurs + 0x6C) == 0xFFFFFFFFu); // wklFlag.flag (CellSpursWorkloadFlag at 0x60, flag at +0xC)
    CHECK(ps_read8(base, kSpurs + 0x77) == 0xFF);       // wklFlagReceiver
    CHECK(ps_read64(base, kSpurs + 0xD20) == 0xFFFFFFFFull); // ppu0
    CHECK(ps_read8(base, kSpurs + 0xC7) == 0xFF);       // sysSrvPreemptWklId[7]
    CHECK(ps_read64(base, kSpurs + 0xD00) == 0x100);    // wklInfoSysSrv.addr
    CHECK(ps_read32(base, kSpurs + 0xD74) == 0x818);    // spuImg.entry_point
    CHECK(ps_read8(base, kSpurs + 0xD9B) == 9);         // prefixSize
    CHECK(std::memcmp(base + kSpurs + 0xD8C, "gta5spurs", 9) == 0);
    CHECK(ps_read8(base, kSpurs + 0x200) == 0);         // the rest of the 4 KB is cleared
    CHECK(ps_read8(base, kSpurs + 0x1000) == 0xAB);     // and nothing past it (SPURS1)
    CHECK(a.call("cellSpursInitializeWithAttribute", kSpurs, kAttrAddr) == kStat); // already initialized

    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
    CHECK(a.call("cellSpursFinalize", kSpurs) == kStat);

    ps_write32(base, kAttrAddr + 0x8, 7); // 7 SPUs: the SPU thread group would refuse it
    CHECK(a.call("cellSpursInitializeWithAttribute", kSpurs, kAttrAddr) == kLv2Inval);
    ps_write32(base, kAttrAddr + 0x8, 6);
    ps_write32(base, kAttrAddr + 0xC, 8); // priority below 16 ...
    CHECK(a.call("cellSpursInitializeWithAttribute", kSpurs, kAttrAddr) == kLv2Inval);
    REQUIRE(a.call("cellSpursAttributeSetSpuThreadGroupType", kAttrAddr, 0x18) == 0); // ... fine without the scheduler
    CHECK(a.call("cellSpursInitializeWithAttribute", kSpurs, kAttrAddr) == 0);
    CHECK(a.call("cellSpursFinalize", kSpurs) == 0);
}

TEST_CASE("sys_ppu_thread_create: TLS, start, entry argument, exit; failed creation returns the TLS slot", "[hle][thread]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);

    // TLS template: 0x20-byte image of 0x5A bytes, 0x40 bytes per thread.
    constexpr u32 kImage = kData + 0x4000;
    std::memset(base + kImage, 0x5A, 0x20);
    a.ctx.gpr[13] = 0;
    REQUIRE(a.call("sys_initialize_tls", 1, kImage, 0x20, 0x40) == a.ctx.gpr[3]);
    const u32 first_slot = static_cast<u32>(a.ctx.gpr[13]) - hle::kTlsR13Offset;

    // Guest thread: std r13, 0(r3); li r3, 0x55; li r11, 41 (_sys_ppu_thread_exit); sc.
    constexpr u32 kCode = kData + 0x3000, kOpd = kData + 0x3100, kOut = kData + 0x3200, kId = kData + 0x3300;
    constexpr u32 kJoined = kData + 0x3400, kName = kData + 0x3500;
    const u32 code[] = {0xF9A30000, 0x38600055, 0x39600029, 0x44000002};
    for (u32 i = 0; i < 4; i++) ps_write32(base, kCode + 4 * i, code[i]);
    ps_write32(base, kOpd, kCode);
    ps_write32(base, kOpd + 4, 0);
    std::memcpy(base + kName, "worker", 7);

    // Failures (priority out of range) must not leak TLS slots.
    for (int i = 0; i < 50; i++) CHECK(a.call("sys_ppu_thread_create", kId, kOpd, kOut, 4000, 0x4000, 1, kName) == r3_of(lv2::CELL_EINVAL));

    REQUIRE(a.call("sys_ppu_thread_create", kId, kOpd, kOut, 1000, 0x4000, 1 /* joinable */, kName) == 0);
    const u32 id = static_cast<u32>(ps_read64(base, kId));
    CHECK(id >= 0x01000000);
    REQUIRE(lv2::sys_ppu_thread_join(*a.t, id, kJoined) == lv2::CELL_OK);
    CHECK(ps_read64(base, kJoined) == 0x55);
    const u64 r13 = ps_read64(base, kOut);
    CHECK(r13 == u64{first_slot} + 0x40 + 0x30 + hle::kTlsR13Offset); // the slot after the caller's, not a leaked one
    CHECK(ps_read8(base, static_cast<u32>(r13) - 0x7000) == 0x5A);     // the image was copied
}

TEST_CASE("sceNpManagerGetStatus before NP initialization", "[hle][np]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    CHECK(a.call("sceNpManagerGetStatus", kData) == 0xFFFFFFFF8002AA01ull); // SCE_NP_ERROR_NOT_INITIALIZED
}

TEST_CASE("cellSpurs tasksets: attributes, creation registers a taskset workload", "[hle][spurs]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kTsAttr = kData + 0x4000, kPrio = kData + 0x4400;
    constexpr u32 kName = kData + 0x4500, kTs0 = kData + 0x6000, kTs1 = kData + 0x8000;
    REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 5, 200, 2016, 0) == 0);
    REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);

    const u8 prio[8] = {1, 1, 1, 1, 1, 0, 0, 0};
    std::memcpy(base + kPrio, prio, 8);
    std::memcpy(base + kName, "physics", 8);
    REQUIRE(a.call("_cellSpursTasksetAttributeInitialize", kTsAttr, 1, 0x330000, 0x1234, kPrio, 5) == 0);
    CHECK(ps_read32(base, kTsAttr + 0x20) == 6400); // taskset_size
    CHECK(ps_read32(base, kTsAttr + 0x18) == 5);    // max_contention
    REQUIRE(a.call("cellSpursTasksetAttributeSetName", kTsAttr, kName) == 0);
    CHECK(ps_read32(base, kTsAttr + 0x1C) == kName);

    REQUIRE(a.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs0, kTsAttr) == 0);
    CHECK(ps_read64(base, kTs0 + 0x60) == kSpurs);
    CHECK(ps_read64(base, kTs0 + 0x68) == 0x1234);
    CHECK(ps_read32(base, kTs0 + 0x74) == 0); // wid
    CHECK(ps_read8(base, kTs0 + 0x72) == 0x80);
    CHECK(ps_read32(base, kTs0 + 0x1890) == 6400);
    CHECK(ps_read32(base, kSpurs + 0xB0) == (0xFFFFu | 0x80000000u)); // wklEnabled: workload 0 claimed
    CHECK(ps_read32(base, kSpurs + 0xB4) == 0x80000000u);             // wklMskB
    CHECK(ps_read8(base, kSpurs + 0x80) == 2);                         // wklState1[0] = RUNNABLE
    CHECK(ps_read64(base, kSpurs + 0xB00) == 0x200);                   // wklInfo1[0].addr: taskset policy marker
    CHECK(ps_read64(base, kSpurs + 0xB08) == kTs0);                    // .arg: the taskset
    CHECK(ps_read32(base, kSpurs + 0xB10) == 0x1E40);
    CHECK(ps_read8(base, kSpurs + 0xB18) == 1);                        // priority[0]
    CHECK(ps_read8(base, kSpurs + 0x40) == 8);                         // min contention
    CHECK(ps_read8(base, kSpurs + 0x50) == 5);                         // max contention
    CHECK(ps_read64(base, kSpurs + 0xE08) == kName);                   // nameInstance

    REQUIRE(a.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs1, kTsAttr) == 0);
    CHECK(ps_read32(base, kTs1 + 0x74) == 1);
    CHECK(ps_read8(base, kSpurs + 0xB20 + 0x14) == ps_read8(base, kSpurs + 0xB00 + 0x14)); // same policy, same unique ID

    // A CellSpursTaskset2 through the attribute's size (as GTA V makes one): libsre also stores the exit code
    // container handler's descriptor at 0x1900 (its marker, 0x710 bytes = 15 units).
    constexpr u32 kTs2 = kData + 0xA000;
    CHECK(a.call("cellSpursTasksetAttributeSetTasksetSize", 0, 10496) == 0xFFFFFFFF80410911ull);
    CHECK(a.call("cellSpursTasksetAttributeSetTasksetSize", kTsAttr + 4, 10496) == 0xFFFFFFFF80410910ull);
    CHECK(a.call("cellSpursTasksetAttributeSetTasksetSize", kTsAttr, 0x2000) == 0xFFFFFFFF80410902ull);
    REQUIRE(a.call("cellSpursTasksetAttributeSetTasksetSize", kTsAttr, 10496) == 0);
    CHECK(ps_read32(base, kTsAttr + 0x20) == 10496);
    std::memset(base + kTs2, 0xEE, 10496);
    REQUIRE(a.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs2, kTsAttr) == 0);
    CHECK(ps_read32(base, kTs2 + 0x1890) == 10496);
    CHECK(ps_read64(base, kTs2 + 0x1900) == (0x400 | 15));
    CHECK(ps_read64(base, kTs2 + 0x1980) == 0); // task_exit_code: cleared with the rest
    CHECK(ps_read32(base, kTs2 + 10496 - 4) == 0);
    CHECK(ps_read32(base, kTs2 + 0x74) == 2);

    ps_write32(base, kTsAttr, 2); // revision
    CHECK(a.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs1, kTsAttr) == 0xFFFFFFFF80410902ull);
    base[kPrio] = 0x10;
    CHECK(a.call("_cellSpursTasksetAttributeInitialize", kTsAttr, 1, 0x330000, 0, kPrio, 5) == 0xFFFFFFFF80410902ull);
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

TEST_CASE("cellSpurs event flags and taskset shutdown; sys_net, sys_spu_initialize, cellSheap", "[hle][spurs]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kTsAttr = kData + 0x4000, kPrio = kData + 0x4400;
    constexpr u32 kTs = kData + 0x6000, kFlag = kData + 0x8000, kHeap = kData + 0x10000;
    constexpr u64 kTaskInval = 0xFFFFFFFF80410902ull, kTaskAlign = 0xFFFFFFFF80410910ull,
                  kTaskNull = 0xFFFFFFFF80410911ull, kTaskStat = 0xFFFFFFFF8041090Full;
    REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 5, 200, 2016, 0) == 0);
    REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
    std::memset(base + kPrio, 1, 8);
    REQUIRE(a.call("_cellSpursTasksetAttributeInitialize", kTsAttr, 1, 0x330000, 0, kPrio, 1) == 0);

    // A taskset that was never created (zeroed) names no SPURS instance: libsre passes cellSpursShutdownWorkload's
    // POLICY_MODULE_ERROR_NULL_POINTER on unchanged.
    std::memset(base + kTs, 0, 0x80);
    CHECK(a.call("cellSpursShutdownTaskset", kTs) == 0xFFFFFFFF80410811ull);
    REQUIRE(a.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs, kTsAttr) == 0);

    // Bink's sequence (run 008): an event flag on the taskset, then shut the taskset down.
    std::memset(base + kFlag, 0xEE, 0x80);
    REQUIRE(a.call("_cellSpursEventFlagInitialize", 0, kTs, kFlag, 1 /* manual */, 2 /* PPU2SPU */) == 0);
    CHECK(ps_read8(base, kFlag + 0x00) == 0);    // events cleared
    CHECK(ps_read8(base, kFlag + 0x0C) == 0xFF); // spuPort: none
    CHECK(ps_read8(base, kFlag + 0x0D) == 0);    // isIwl
    CHECK(ps_read8(base, kFlag + 0x0E) == 2);    // direction
    CHECK(ps_read8(base, kFlag + 0x0F) == 1);    // clearMode
    CHECK(ps_read64(base, kFlag + 0x70) == kTs); // addr: the taskset
    CHECK(ps_read8(base, kFlag + 0x7F) == 0);
    REQUIRE(a.call("_cellSpursEventFlagInitialize", kSpurs, 0, kFlag, 0, 3) == 0);
    CHECK(ps_read8(base, kFlag + 0x0D) == 1);    // isIwl: attached to the SPURS instance
    CHECK(ps_read64(base, kFlag + 0x70) == kSpurs);
    CHECK(a.call("_cellSpursEventFlagInitialize", 0, 0, kFlag, 0, 0) == kTaskNull);
    CHECK(a.call("_cellSpursEventFlagInitialize", 0, kTs, kFlag + 0x40, 0, 0) == kTaskAlign);
    CHECK(a.call("_cellSpursEventFlagInitialize", 0, kTs, kFlag, 2, 0) == kTaskInval);
    CHECK(a.call("_cellSpursEventFlagInitialize", 0, kTs, kFlag, 0, 4) == kTaskInval);

    REQUIRE(a.call("cellSpursShutdownTaskset", kTs) == 0);
    CHECK(ps_read8(base, kSpurs + 0x80) == 4); // wklState1[0]: REMOVABLE (no SPU was running it)
    CHECK(ps_read8(base, kSpurs + 0xA0) == 1); // wklEvent1[0]: shutdown bit
    CHECK(a.call("cellSpursShutdownTaskset", kTs) == 0); // already removable
    // libsre: a workload ID past 31 is the taskset's INVAL; 16..31 is past a SPURS1 instance, the workload's INVAL
    // (passed on); the instance's STAT (here: exception state) becomes the taskset's.
    constexpr u32 kOtherTs = kData + 0xA000;
    std::memset(base + kOtherTs, 0, 0x80);
    ps_write64(base, kOtherTs + 0x60, kSpurs);
    ps_write32(base, kOtherTs + 0x74, 32);
    CHECK(a.call("cellSpursShutdownTaskset", kOtherTs) == kTaskInval);
    ps_write32(base, kOtherTs + 0x74, 16);
    CHECK(a.call("cellSpursShutdownTaskset", kOtherTs) == 0xFFFFFFFF80410802ull);
    CHECK(a.call("cellSpursShutdownTaskset", kOtherTs + 0x40) == kTaskAlign);
    ps_write32(base, kSpurs + 0xD6C, 1); // CellSpurs::exception
    CHECK(a.call("cellSpursShutdownTaskset", kTs) == kTaskStat);
    CHECK(a.call("cellSpursJoinTaskset", kTs) == kTaskStat);
    ps_write32(base, kSpurs + 0xD6C, 0);

    // cellSpursJoinTaskset (D-136): the shutdown's checks, then the workload is removed and its slot is free again.
    CHECK(a.call("cellSpursJoinTaskset", 0) == kTaskNull);
    CHECK(a.call("cellSpursJoinTaskset", kTs + 0x40) == kTaskAlign);
    ps_write32(base, kOtherTs + 0x74, 32);
    CHECK(a.call("cellSpursJoinTaskset", kOtherTs) == kTaskInval);
    ps_write32(base, kOtherTs + 0x74, 16);
    CHECK(a.call("cellSpursJoinTaskset", kOtherTs) == 0xFFFFFFFF80410802ull);
    CHECK(a.call("cellSpursJoinTaskset", kTs) == 0);
    CHECK(ps_read8(base, kSpurs + 0x80) == 0);                 // wklState1[0]: NON_EXISTENT
    CHECK((ps_read32(base, kSpurs + 0xB0) & 0x80000000u) == 0); // wklEnabled
    CHECK((ps_read32(base, kSpurs + 0xB4) & 0x80000000u) == 0); // wklMskB
    CHECK(a.call("cellSpursJoinTaskset", kTs) == 0xFFFFFFFF80410805ull); // POLICY_MODULE_ERROR_SRCH: no workload 0
    REQUIRE(a.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs, kTsAttr) == 0);
    CHECK(ps_read32(base, kTs + 0x74) == 0); // the free slot again
    CHECK(ps_read8(base, kSpurs + 0x80) == 2); // RUNNABLE
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);

    ps_write32(base, kData + 0x100, kData + 0x1F0000); // sys_net_initialize_parameter_t {memory, size, flags}
    ps_write32(base, kData + 0x104, 0x10000);
    ps_write32(base, kData + 0x108, 0);
    CHECK(a.call("sys_net_initialize_network_ex", kData + 0x100) == 0);
    a.call("_sys_process_atexitspawn", 0x10000);
    a.call("_sys_process_at_Exitspawn", 0x10008);

    CHECK(a.call("cellSheapInitialize", kHeap, 0x80000, 0, 0) == 0);
    CHECK(a.call("cellSheapInitialize", kHeap + 0x40, 0x80000, 0, 0) == 0xFFFFFFFF80410310ull); // ALIGN
    CHECK(a.call("cellSheapInitialize", 0, 0x80000, 0, 0) == 0xFFFFFFFF80410302ull);           // INVAL

    a.ctx.gpr[11] = 169; // sys_spu_initialize(6, 0) as GTA V calls it, then an invalid raw SPU count
    a.ctx.gpr[3] = 6;
    a.ctx.gpr[4] = 0;
    ps_ppu_syscall(&a.ctx, base);
    CHECK(a.ctx.gpr[3] == 0);
    a.ctx.gpr[11] = 169;
    a.ctx.gpr[3] = 6;
    a.ctx.gpr[4] = 6;
    ps_ppu_syscall(&a.ctx, base);
    CHECK(a.ctx.gpr[3] == 0xFFFFFFFF80010002ull);
}

TEST_CASE("cellNetCtl: init once, disconnected, handlers accepted", "[hle][net]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    constexpr u64 kNotInit = 0xFFFFFFFF80130101ull, kNotTerm = 0xFFFFFFFF80130102ull, kBadAddr = 0xFFFFFFFF80130107ull;
    CHECK(a.call("cellNetCtlGetState", kData) == kNotInit);
    REQUIRE(a.call("cellNetCtlInit") == 0);
    CHECK(a.call("cellNetCtlInit") == kNotTerm);
    ps_write32(f.base(), kData, 0xFFFFFFFF);
    REQUIRE(a.call("cellNetCtlGetState", kData) == 0);
    CHECK(ps_read32(f.base(), kData) == 0); // CELL_NET_CTL_STATE_Disconnected
    CHECK(a.call("cellNetCtlGetState", 0) == kBadAddr);
    CHECK(a.call("cellNetCtlAddHandler", 0x10000, 0, kData + 8) == 0);
    CHECK(a.call("cellNetCtlDelHandler", 0) == 0);
    CHECK(a.call("cellNetCtlDelHandler", 4) == 0xFFFFFFFF80130105ull);
    a.call("cellNetCtlTerm");
    CHECK(a.call("cellNetCtlInit") == 0);
}

TEST_CASE("cellSpurs job chains: 14-argument attribute (stack arguments), creation registers a job chain workload", "[hle][spurs]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kJcAttr = kData + 0x4000, kPrio = kData + 0x4400;
    constexpr u32 kEntry = kData + 0x4800, kName = kData + 0x4900, kJc = kData + 0x6000;
    constexpr u64 kJobInval = 0xFFFFFFFF80410A02ull;
    REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 5, 200, 2016, 0) == 0);
    REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
    const u8 prio[8] = {2, 2, 2, 2, 2, 0, 0, 0};
    std::memcpy(base + kPrio, prio, 8);
    std::memcpy(base + kName, "physics", 8);

    // jm 3, SDK 3.30, attr, entry, 128-byte jobs, 4 grabbed, priorities, contention 5, then on the stack: autoSpuCount 1,
    // tags 20/21, fixed memory 1, max job descriptor 0x200, initial SPU count 3.
    REQUIRE(a.call("_cellSpursJobChainAttributeInitialize", 3, 0x330000, kJcAttr, kEntry, 0x80, 4, kPrio, 5, 1, 20, 21, 1,
                0x200, 3) == 0);
    CHECK(ps_read32(base, kJcAttr + 0x00) == 3);
    CHECK(ps_read32(base, kJcAttr + 0x08) == kEntry);
    CHECK(ps_read16(base, kJcAttr + 0x0C) == 0x80);
    CHECK(ps_read16(base, kJcAttr + 0x0E) == 4);
    CHECK(ps_read8(base, kJcAttr + 0x10) == 2);
    CHECK(ps_read32(base, kJcAttr + 0x18) == 5);
    CHECK(ps_read8(base, kJcAttr + 0x1C) == 1);     // autoSpuCount (stack argument 9)
    CHECK(ps_read32(base, kJcAttr + 0x20) == 20);   // tag1
    CHECK(ps_read32(base, kJcAttr + 0x24) == 21);   // tag2
    CHECK(ps_read8(base, kJcAttr + 0x28) == 1);     // isFixedMemAlloc
    CHECK(ps_read32(base, kJcAttr + 0x2C) == 0x200); // maxSizeJobDescriptor
    CHECK(ps_read32(base, kJcAttr + 0x30) == 3);    // initSpuCount (stack argument 14)
    REQUIRE(a.call("cellSpursJobChainAttributeSetName", kJcAttr, kName) == 0);
    REQUIRE(a.call("cellSpursJobChainAttributeSetHaltOnError", kJcAttr) == 0);
    CHECK(ps_read32(base, kJcAttr + 0x34) == kName); // libsre: name at 0x34, haltOnError at 0x38 (RPCS3 swaps them)
    CHECK(ps_read32(base, kJcAttr + 0x38) == 1);

    REQUIRE(a.call("cellSpursCreateJobChainWithAttribute", kSpurs, kJc, kJcAttr) == 0);
    CHECK(ps_read64(base, kJc + 0x00) == kEntry);  // pc
    CHECK(ps_read64(base, kJc + 0x78) == kSpurs);
    CHECK(ps_read32(base, kJc + 0x74) == 0);       // workloadId
    CHECK(ps_read16(base, kJc + 0x70) == 0x80);    // sizeJobDescriptor (libsre; RPCS3 puts maxGrabbedJob here)
    CHECK(ps_read16(base, kJc + 0x72) == 4);       // maxGrabbedJob
    CHECK(ps_read8(base, kJc + 0x24) == 1);        // autoReadyCount
    CHECK(ps_read8(base, kJc + 0x2A) == 20);
    CHECK(ps_read8(base, kJc + 0x2B) == 21);
    CHECK(ps_read8(base, kJc + 0x2C) == (0x80 | (2 << 4) | 0x04)); // fixed memory, (0x200 - 0x100) / 128, haltOnError
    CHECK(ps_read8(base, kJc + 0x2D) == 3);        // jmVer from the attribute
    CHECK(ps_read8(base, kJc + 0x28) == 3);        // initSpuCount
    CHECK(ps_read32(base, kJc + 0x90) == 0x330000);
    CHECK(ps_read64(base, kSpurs + 0xB00) == 0x300); // the native job chain marker
    CHECK(ps_read64(base, kSpurs + 0xB08) == kJc);
    CHECK(ps_read8(base, kSpurs + 0x40) == 1);       // min contention 1
    CHECK(ps_read8(base, kSpurs + 0x50) == 5);

    // Validation (RPCS3 check_job_chain_attribute).
    CHECK(a.call("_cellSpursJobChainAttributeInitialize", 3, 0x330000, kJcAttr, kEntry, 0x80, 0, kPrio, 5, 0, 0, 0, 0,
              0x200, 0) == kJobInval); // no grabbed jobs
    CHECK(a.call("_cellSpursJobChainAttributeInitialize", 3, 0x330000, kJcAttr, kEntry, 0x80, 4, kPrio, 5, 0, 31, 0, 0,
              0x200, 0) == kJobInval); // tag 31 after SDK 1.90
    CHECK(a.call("_cellSpursJobChainAttributeInitialize", 3, 0x330000, kJcAttr, kEntry, 0x80, 4, kPrio, 5, 0, 0, 0, 0,
              0x80, 0) == kJobInval); // max job descriptor too small
    ps_write32(base, kJcAttr, 1); // jm revision 1 can't be created with an attribute
    CHECK(a.call("cellSpursCreateJobChainWithAttribute", kSpurs, kJc + 0x200, kJcAttr) == kJobInval);

    // cellSpursRunJobChain, as libsre: checks, then a workload signal (wklSignal1 bit 0x8000 >> wid).
    CHECK(a.call("cellSpursRunJobChain", 0) == 0xFFFFFFFF80410A11ull);
    CHECK(a.call("cellSpursRunJobChain", kJc + 8) == 0xFFFFFFFF80410A10ull);
    REQUIRE(a.call("cellSpursRunJobChain", kJc) == 0);
    CHECK(ps_read16(base, kSpurs + 0x70) == 0x8000);
    ps_write8(base, kJc + 0x2D, 1); // revision 1: PERM
    CHECK(a.call("cellSpursRunJobChain", kJc) == 0xFFFFFFFF80410A09ull);
    ps_write8(base, kJc + 0x2D, 3);
    ps_write32(base, kJc + 0x74, 32); // workload ID past 31: INVAL
    CHECK(a.call("cellSpursRunJobChain", kJc) == kJobInval);
    ps_write32(base, kJc + 0x74, 0);
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

TEST_CASE("cellSpurs event flags: lv2 queue attach, PPU set and wait, detach; SPURS queue ports; barriers",
    "[hle][spurs]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kTsAttr = kData + 0x4000, kPrio = kData + 0x4400;
    constexpr u32 kTs = kData + 0x6000, kFlag = kData + 0x8000, kFlag2 = kData + 0x8080, kFlag3 = kData + 0x8100;
    constexpr u32 kMask = kData + 0x8200, kMaskB = kData + 0x8210, kQueueOut = kData + 0x8220,
                  kQueueAttr = kData + 0x8230, kPort = kData + 0x8240, kBarrier = kData + 0x8280;
    constexpr u64 kTaskStat = 0xFFFFFFFF8041090Full, kTaskPerm = 0xFFFFFFFF80410909ull,
                  kTaskBusy = 0xFFFFFFFF8041090Aull, kTaskInval = 0xFFFFFFFF80410902ull,
                  kCoreBusy = 0xFFFFFFFF8041070Aull, kCorePerm = 0xFFFFFFFF80410709ull,
                  kCoreSrch = 0xFFFFFFFF80410705ull, kCoreInval = 0xFFFFFFFF80410702ull;
    REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 5, 200, 2016, 0) == 0);
    REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
    std::memset(base + kPrio, 1, 8);
    REQUIRE(a.call("_cellSpursTasksetAttributeInitialize", kTsAttr, 1, 0x330000, 0, kPrio, 1) == 0);
    REQUIRE(a.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs, kTsAttr) == 0);

    // GTA V's sequence (run 011): an event flag on a taskset, then its lv2 event queue.
    REQUIRE(a.call("_cellSpursEventFlagInitialize", 0, kTs, kFlag, 0 /* auto */, 3 /* ANY2ANY */) == 0);
    REQUIRE(a.call("cellSpursEventFlagAttachLv2EventQueue", kFlag) == 0);
    CHECK(ps_read8(base, kFlag + 0x0C) == 0x10); // spuPort: the first dynamic port
    const u32 queue = ps_read32(base, kFlag + 0x7C);
    CHECK(f.kernel->event_queue(queue) != nullptr);
    CHECK(ps_read32(base, kFlag + 0x78) != 0);   // eventPortId: ANY2ANY flags get a local port
    CHECK(ps_read64(base, kSpurs + 0xDA8) == 0); // spuPortBits: SPURS' own queues don't show
    CHECK(a.call("cellSpursEventFlagAttachLv2EventQueue", kFlag) == kTaskStat); // already attached

    REQUIRE(a.call("_cellSpursEventFlagInitialize", 0, kTs, kFlag2, 1, 1 /* SPU2PPU */) == 0);
    REQUIRE(a.call("cellSpursEventFlagAttachLv2EventQueue", kFlag2) == 0);
    CHECK(ps_read8(base, kFlag2 + 0x0C) == 0x11);
    CHECK(ps_read32(base, kFlag2 + 0x78) == 0); // no local port
    REQUIRE(a.call("_cellSpursEventFlagInitialize", 0, kTs, kFlag3, 1, 2 /* PPU2SPU */) == 0);
    CHECK(a.call("cellSpursEventFlagAttachLv2EventQueue", kFlag3) == kTaskPerm);
    CHECK(a.call("cellSpursEventFlagWait", kFlag3, kMask, 0) == kTaskPerm);
    // libsre's cellSpursEventFlagSet takes PPU2SPU and ANY2ANY flags (RPCS3 checks SPU2PPU, as for a wait).
    CHECK(a.call("cellSpursEventFlagSet", kFlag2, 0x0001) == kTaskPerm);
    REQUIRE(a.call("cellSpursEventFlagSet", kFlag3, 0x0001) == 0);
    CHECK(ps_read16(base, kFlag3) == 0x0001);

    // Bits already set: no blocking; AUTO clears the bits the waiter consumed.
    REQUIRE(a.call("cellSpursEventFlagSet", kFlag, 0x0003) == 0);
    CHECK(ps_read16(base, kFlag) == 0x0003);
    ps_write16(base, kMask, 0x0001);
    REQUIRE(a.call("cellSpursEventFlagWait", kFlag, kMask, 0 /* OR */) == 0);
    CHECK(ps_read16(base, kMask) == 0x0001);
    CHECK(ps_read16(base, kFlag) == 0x0002);
    CHECK(a.call("cellSpursEventFlagWait", kFlag, kMask, 2) == kTaskInval); // mode

    // A PPU thread blocks (AND 0x000C) until both bits are set, then gets them.
    std::atomic<u64> b_rc{~0ull};
    std::atomic<u32> b_mask{0};
    ps_write16(base, kMaskB, 0x000C);
    std::thread tb([&] {
        GuestThread b(f, 1, "b");
        b_rc = b.call("cellSpursEventFlagWait", kFlag, kMaskB, 1 /* AND */);
        b_mask = ps_read16(base, kMaskB);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    // ppuWaitMask, which b's wait sets with a reservation operation: read through the reservation system (D-585).
    while (hle::test::line_half(base, kFlag + 0x04) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(hle::test::line_half(base, kFlag + 0x04) == 0x000C); // ppuWaitMask
    CHECK(ps_read8(base, kFlag + 0x06) == 0xF1);       // wait slot 15 (no SPU task holds one), mode AND
    CHECK(a.call("cellSpursEventFlagWait", kFlag, kMask, 0) == kTaskBusy); // one PPU waiter at a time
    REQUIRE(a.call("cellSpursEventFlagSet", kFlag, 0x0004) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(b_rc.load() == ~0ull); // still waiting for 0x0008
    CHECK(a.call("cellSpursEventFlagDetachLv2EventQueue", kFlag) == kTaskBusy);
    REQUIRE(a.call("cellSpursEventFlagSet", kFlag, 0x0008) == 0);
    tb.join();
    CHECK(b_rc.load() == 0);
    CHECK(b_mask.load() == 0x000C);
    CHECK(ps_read16(base, kFlag) == 0x0002);     // 0x000C consumed (AUTO)
    CHECK(ps_read16(base, kFlag + 0x04) == 0);   // no waiter
    CHECK(ps_read8(base, kFlag + 0x07) == 0);    // ppuPendingRecv

    // Detach destroys the queue and frees the port for the next attach.
    REQUIRE(a.call("cellSpursEventFlagDetachLv2EventQueue", kFlag) == 0);
    CHECK(ps_read8(base, kFlag + 0x0C) == 0xFF);
    CHECK(f.kernel->event_queue(queue) == nullptr);
    CHECK(a.call("cellSpursEventFlagDetachLv2EventQueue", kFlag) == kTaskStat);
    CHECK(a.call("cellSpursEventFlagWait", kFlag, kMask, 0) == kTaskStat); // blocking needs the queue
    REQUIRE(a.call("cellSpursEventFlagAttachLv2EventQueue", kFlag) == 0);
    CHECK(ps_read8(base, kFlag + 0x0C) == 0x10);

    // cellSpursAttachLv2EventQueue: the game's own queue on a dynamic or given SPU port (spuPortBits records both).
    ps_write32(base, kQueueAttr, 2);     // SYS_SYNC_PRIORITY
    ps_write32(base, kQueueAttr + 4, 1); // SYS_PPU_QUEUE
    REQUIRE(lv2::sys_event_queue_create(*a.t, kQueueOut, kQueueAttr, 0, 8) == 0);
    const u32 game_queue = ps_read32(base, kQueueOut);
    REQUIRE(a.call("cellSpursAttachLv2EventQueue", kSpurs, game_queue, kPort, 1) == 0);
    CHECK(ps_read8(base, kPort) == 0x12);
    base[kPort] = 5;
    REQUIRE(a.call("cellSpursAttachLv2EventQueue", kSpurs, game_queue, kPort, 0) == 0);
    CHECK(ps_read8(base, kPort) == 5);
    CHECK(ps_read64(base, kSpurs + 0xDA8) == ((1ull << 0x12) | (1ull << 5)));
    CHECK(a.call("cellSpursAttachLv2EventQueue", kSpurs, game_queue, kPort, 0) == kCoreBusy); // port 5 taken
    base[kPort] = 0x20;
    CHECK(a.call("cellSpursAttachLv2EventQueue", kSpurs, game_queue, kPort, 0) == kCorePerm); // > 0xF from SDK 1.8
    base[kPort] = 6;
    CHECK(a.call("cellSpursAttachLv2EventQueue", kSpurs, 0x12345678, kPort, 0) == r3_of(lv2::CELL_ESRCH));
    REQUIRE(a.call("cellSpursDetachLv2EventQueue", kSpurs, 5) == 0);
    CHECK(ps_read64(base, kSpurs + 0xDA8) == (1ull << 0x12));
    CHECK(a.call("cellSpursDetachLv2EventQueue", kSpurs, 5) == kCoreSrch);
    CHECK(a.call("cellSpursDetachLv2EventQueue", kSpurs, 0x40) == kCoreInval);
    base[kPort] = 5;
    CHECK(a.call("cellSpursAttachLv2EventQueue", kSpurs, game_queue, kPort, 0) == 0); // free again after detach

    REQUIRE(a.call("cellSpursBarrierInitialize", kTs, kBarrier, 6) == 0);
    CHECK(ps_read32(base, kBarrier + 0x04) == 6);
    CHECK(ps_read32(base, kBarrier + 0x34) == kTs);
    CHECK(a.call("cellSpursBarrierInitialize", kTs, kBarrier, 0) == kTaskInval);
    CHECK(a.call("cellSpursBarrierInitialize", kTs, kBarrier, 129) == kTaskInval);
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

// A synthetic SPU ELF (32-bit big-endian, EM_SPU) with the given entry point and program headers.
struct TestSpuSegment {
    u32 type, vaddr, filesz, memsz, flags, paddr;
};

template <usize N>
static void write_spu_elf(u8* base, u32 elf, u32 entry, const TestSpuSegment (&segs)[N])
{
    std::memset(base + elf, 0, 0x34 + 0x20 * N);
    ps_write32(base, elf, 0x7F454C46);
    base[elf + 4] = 1; // ELFCLASS32
    base[elf + 5] = 2; // big-endian
    base[elf + 6] = 1; // EV_CURRENT
    ps_write16(base, elf + 0x12, 23); // EM_SPU
    ps_write32(base, elf + 0x18, entry);
    ps_write32(base, elf + 0x1C, 0x34);
    ps_write16(base, elf + 0x2A, 0x20);
    ps_write16(base, elf + 0x2C, static_cast<u16>(N));
    u32 ph = elf + 0x34;
    for (const TestSpuSegment& s : segs) {
        ps_write32(base, ph + 0x00, s.type);
        ps_write32(base, ph + 0x08, s.vaddr);
        ps_write32(base, ph + 0x0C, s.paddr);
        ps_write32(base, ph + 0x10, s.filesz);
        ps_write32(base, ph + 0x14, s.memsz);
        ps_write32(base, ph + 0x18, s.flags);
        ph += 0x20;
    }
}

// A synthetic SPU task ELF: text from `text_vaddr` (0x1900 bytes, read-only), data 0x4900..0x5000 (writable), and a
// NOTE segment the patterns ignore.
static void write_task_elf(u8* base, u32 elf, u32 text_vaddr = 0x3000, u32 entry = 0x3000)
{
    write_spu_elf(base, elf, entry,
        {{1, text_vaddr, 0x1900, 0x1900, 5, text_vaddr}, {1, 0x4900, 0x700, 0x700, 6, 0x4900}, {4, 0, 0x40, 0x40, 4, 0}});
}

TEST_CASE("cellSpurs LS patterns as libsre: segment patterns, generated ranges, context save area size",
    "[hle][spurs]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kElf = kData + 0x8000, kPattern = kData + 0x8200, kRo = kData + 0x8210, kSize = kData + 0x8220,
                  kElf2 = kData + 0x8400, kElf3 = kData + 0x8600;
    constexpr u64 kTaskInval = 0xFFFFFFFF80410902ull, kTaskAlign = 0xFFFFFFFF80410910ull,
                  kTaskNoexec = 0xFFFFFFFF80410907ull, kTaskNull = 0xFFFFFFFF80410911ull;
    write_task_elf(base, kElf);

    // One bit per 2 KB block, block 0 the top bit: blocks 6..9 are LS 0x3000..0x5000.
    REQUIRE(a.call("cellSpursTaskGetLoadableSegmentPattern", kPattern, kElf) == 0);
    CHECK(ps_read32(base, kPattern) == 0x03C00000u);
    CHECK(ps_read32(base, kPattern + 4) == 0);
    CHECK(ps_read64(base, kPattern + 8) == 0);
    REQUIRE(a.call("cellSpursTaskGetReadOnlyAreaPattern", kRo, kElf) == 0);
    CHECK(ps_read32(base, kRo) == 0x03800000u); // block 9 holds writable data too, so it is not read-only

    // libsre: read-only blocks come from the file image (filesz); segments whose paddr is outside LS are skipped.
    write_spu_elf(base, kElf2, 0x3000,
        {{1, 0x3000, 0x1000, 0x2000, 5, 0x3000}, {1, 0x6000, 0x1000, 0x1000, 4, 0x40000}, {1, 0x8000, 0x800, 0x800, 6, 0x8000}});
    REQUIRE(a.call("cellSpursTaskGetLoadableSegmentPattern", kPattern, kElf2) == 0);
    CHECK(ps_read32(base, kPattern) == 0x03C08000u); // blocks 6..9 (memsz) and 16 (the writable segment)
    REQUIRE(a.call("cellSpursTaskGetReadOnlyAreaPattern", kRo, kElf2) == 0);
    CHECK(ps_read32(base, kRo) == 0x03000000u); // blocks 6, 7 (filesz)
    // Any program header ending past local store is INVAL, whatever its type.
    write_spu_elf(base, kElf3, 0x3000, {{1, 0x3000, 0x800, 0x800, 5, 0x3000}, {4, 0x3FF00, 0x200, 0x200, 4, 0}});
    CHECK(a.call("cellSpursTaskGetLoadableSegmentPattern", kPattern, kElf3) == kTaskInval);
    CHECK(a.call("cellSpursTaskGetReadOnlyAreaPattern", kRo, kElf3) == kTaskInval);

    // Segments below CELL_SPURS_TASK_TOP are not checked here: their blocks are set (task creation refuses the ELF).
    write_task_elf(base, kElf2, 0x2000);
    REQUIRE(a.call("cellSpursTaskGetLoadableSegmentPattern", kPattern, kElf2) == 0);
    CHECK(ps_read32(base, kPattern) == 0x0F400000u); // blocks 4..7 and 9
    // The header checks: magic (4 bytes), EM_SPU, an entry point in the task area; no alignment check.
    CHECK(a.call("cellSpursTaskGetLoadableSegmentPattern", kPattern, kElf + 8) == kTaskNoexec);
    write_task_elf(base, kElf2, 0x3000, 0x2FF0);
    CHECK(a.call("cellSpursTaskGetLoadableSegmentPattern", kPattern, kElf2) == kTaskNoexec);
    write_task_elf(base, kElf2);
    base[kElf2 + 7] = 0x66; // OS ABI: these helpers check only the magic
    CHECK(a.call("cellSpursTaskGetReadOnlyAreaPattern", kRo, kElf2) == 0);
    ps_write16(base, kElf2 + 0x12, 21);
    CHECK(a.call("cellSpursTaskGetReadOnlyAreaPattern", kRo, kElf2) == kTaskNoexec);
    // NULL ELF first, the pattern only after the ELF's header passed.
    CHECK(a.call("cellSpursTaskGetLoadableSegmentPattern", 0, 0) == kTaskNull);
    CHECK(a.call("cellSpursTaskGetLoadableSegmentPattern", 0, kElf2) == kTaskNoexec);
    CHECK(a.call("cellSpursTaskGetLoadableSegmentPattern", 0, kElf) == kTaskNull);

    // cellSpursTaskGenerateLsPattern: 2 KB aligned, ending at or below 0x40000. libsre does not refuse SPURS' own
    // blocks here.
    REQUIRE(a.call("cellSpursTaskGenerateLsPattern", kRo, 0x3F000, 0x1000) == 0);
    CHECK(ps_read64(base, kRo) == 0);
    CHECK(ps_read64(base, kRo + 8) == 3); // blocks 126, 127
    REQUIRE(a.call("cellSpursTaskGenerateLsPattern", kRo, 0x2800, 0x800) == 0);
    CHECK(ps_read32(base, kRo) == 0x04000000u); // block 5
    REQUIRE(a.call("cellSpursTaskGenerateLsPattern", kRo, 0x7800, 0x20000) == 0); // blocks 15..78: both halves
    CHECK(ps_read64(base, kRo) == 0x0001FFFFFFFFFFFFull);
    CHECK(ps_read64(base, kRo + 8) == 0xFFFE000000000000ull);
    CHECK(a.call("cellSpursTaskGenerateLsPattern", kRo, 0x3100, 0x800) == kTaskAlign);
    CHECK(a.call("cellSpursTaskGenerateLsPattern", kRo, 0x3000, 0x900) == kTaskAlign);
    CHECK(a.call("cellSpursTaskGenerateLsPattern", kRo, 0x3F800, 0x1000) == kTaskInval);
    CHECK(a.call("cellSpursTaskGenerateLsPattern", 0, 0x3000, 0x800) == kTaskNull);

    // Context save area: 1 KB plus 2 KB per block; reserved blocks are INVAL.
    write_task_elf(base, kElf);
    REQUIRE(a.call("cellSpursTaskGetLoadableSegmentPattern", kPattern, kElf) == 0);
    REQUIRE(a.call("cellSpursTaskGetContextSaveAreaSize", kSize, kPattern) == 0);
    CHECK(ps_read32(base, kSize) == 0x400 + 4 * 0x800);
    ps_write32(base, kRo, 0x80000000u);
    CHECK(a.call("cellSpursTaskGetContextSaveAreaSize", kSize, kRo) == kTaskInval);
    ps_write64(base, kRo, 0);
    ps_write64(base, kRo + 8, 0);
    REQUIRE(a.call("cellSpursTaskGetContextSaveAreaSize", kSize, kRo) == 0);
    CHECK(ps_read32(base, kSize) == 0x400);
}

TEST_CASE("cellSpurs tasks as libsre: create, task attribute (save config), checks and their order",
    "[hle][spurs]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kTsAttr = kData + 0x4000, kPrio = kData + 0x4400;
    constexpr u32 kTs = kData + 0x6000, kElf = kData + 0x8000, kPattern = kData + 0x8200, kId = kData + 0x8230,
                  kArgs = kData + 0x8240, kSaveConfig = kData + 0x8260, kTaskAttr = kData + 0x8300,
                  kElf2 = kData + 0x8600, kContext = kData + 0x9000;
    constexpr u64 kTaskInval = 0xFFFFFFFF80410902ull, kTaskAlign = 0xFFFFFFFF80410910ull,
                  kTaskNoexec = 0xFFFFFFFF80410907ull, kTaskFault = 0xFFFFFFFF8041090Dull,
                  kTaskAgain = 0xFFFFFFFF80410901ull, kTaskNull = 0xFFFFFFFF80410911ull,
                  kTaskPerm = 0xFFFFFFFF80410909ull;
    REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 5, 200, 2016, 0) == 0);
    REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
    std::memset(base + kPrio, 1, 8);
    REQUIRE(a.call("_cellSpursTasksetAttributeInitialize", kTsAttr, 1, 0x330000, 0, kPrio, 4) == 0);
    REQUIRE(a.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs, kTsAttr) == 0);
    write_task_elf(base, kElf);
    REQUIRE(a.call("cellSpursTaskGetLoadableSegmentPattern", kPattern, kElf) == 0); // blocks 6..9

    // cellSpursCreateTask: the lowest free ID, TaskInfo filled, pending-ready, the workload signalled.
    for (u32 i = 0; i < 16; i++) base[kArgs + i] = static_cast<u8>(0xA0 + i);
    REQUIRE(a.call("cellSpursCreateTask", kTs, kId, kElf, kContext, 0x2400, kPattern, kArgs) == 0);
    CHECK(ps_read32(base, kId) == 0);
    CHECK(ps_read32(base, kTs + 0x30) == 0x80000000u); // enabled
    CHECK(ps_read32(base, kTs + 0x20) == 0x80000000u); // pending_ready
    CHECK(std::memcmp(base + kTs + 0x80, base + kArgs, 16) == 0);
    CHECK(ps_read64(base, kTs + 0x90) == kElf);
    CHECK(ps_read64(base, kTs + 0x98) == (u64{kContext} | 4)); // context | LS blocks the save area has room for
    CHECK(ps_read32(base, kTs + 0xA0) == 0x03C00000u);
    CHECK(ps_read16(base, kSpurs + 0x70) == 0x8000);  // wklSignal1: workload 0
    CHECK(ps_read8(base, kSpurs + 0xD64) == 1);       // handlerDirty

    REQUIRE(a.call("cellSpursCreateTask", kTs, kId, kElf, 0, 0, 0, 0) == 0); // no context, no argument
    CHECK(ps_read32(base, kId) == 1);
    CHECK(ps_read64(base, kTs + 0x80 + 0x30 + 0x18) == 0);
    CHECK(ps_read32(base, kTs + 0x20) == 0xC0000000u);

    // libsre's checks, in its order: ID and taskset pointers, taskset alignment, ELF NULL, ELF and context alignment
    // (context 128-byte from SDK 0x280000), context size, the ELF header, the pattern against the context.
    ps_write32(base, kId, 0x55);
    CHECK(a.call("cellSpursCreateTask", kTs, 0, 0, 0, 0, 0, 0) == kTaskNull);
    CHECK(a.call("cellSpursCreateTask", 0, kId, kElf, 0, 0, 0, 0) == kTaskNull);
    CHECK(a.call("cellSpursCreateTask", kTs + 0x40, kId, 0, 0, 0, 0, 0) == kTaskAlign);
    CHECK(a.call("cellSpursCreateTask", kTs, kId, 0, kContext + 0x40, 0, 0, 0) == kTaskNull);
    CHECK(a.call("cellSpursCreateTask", kTs, kId, kElf + 8, 0, 0, 0, 0) == kTaskAlign);
    CHECK(a.call("cellSpursCreateTask", kTs, kId, kElf, kContext + 0x40, 0x2400, 0, 0) == kTaskAlign);
    CHECK(a.call("cellSpursCreateTask", kTs, kId, kElf, kContext, 0x3FF, 0, 0) == kTaskInval); // context too small
    CHECK(a.call("cellSpursCreateTask", kTs, kId, kElf, kContext, 0x1C00, kPattern, 0) == kTaskInval); // 3 blocks < 4
    write_task_elf(base, kElf2);
    base[kElf2 + 7] = 0x66; // task creation checks e_ident's first 8 bytes (OS ABI 0)
    CHECK(a.call("cellSpursCreateTask", kTs, kId, kElf2, 0, 0, 0, 0) == kTaskNoexec);
    write_task_elf(base, kElf2, 0x2000); // a PT_LOAD segment below CELL_SPURS_TASK_TOP
    CHECK(a.call("cellSpursCreateTask", kTs, kId, kElf2, 0, 0, 0, 0) == kTaskNoexec);
    write_task_elf(base, kElf2, 0x3000, 0x2FFC); // entry point below it
    CHECK(a.call("cellSpursCreateTask", kTs, kId, kElf2, 0, 0, 0, 0) == kTaskNoexec);
    ps_write32(base, kPattern + 12, 0xFFFFFFFFu); // too many blocks for the context, refused with one ...
    CHECK(a.call("cellSpursCreateTask", kTs, kId, kElf, kContext, 0x2400, kPattern, 0) == kTaskInval);
    REQUIRE(a.call("cellSpursCreateTask", kTs, kId, kElf, 0, 0, kPattern, 0) == 0); // ... and not read without one
    CHECK(ps_read32(base, kId) == 2);
    CHECK(ps_read64(base, kTs + 0x80 + 2 * 0x30 + 0x20) == 0);
    ps_write32(base, kPattern + 12, 0);

    // _cellSpursTaskAttributeInitialize(attr, revision, sdkVersion, elf, saveConfig, argument): libsre's six
    // arguments, a CellSpursTaskSaveConfig {context, size, pattern} and libsre's attribute layout.
    ps_write32(base, kSaveConfig + 0, kContext);
    ps_write32(base, kSaveConfig + 4, 0x2400);
    ps_write32(base, kSaveConfig + 8, kPattern);
    std::memset(base + kTaskAttr, 0xCC, 0x100);
    REQUIRE(a.call("_cellSpursTaskAttributeInitialize", kTaskAttr, 1, 0x330000, kElf, kSaveConfig, kArgs) == 0);
    CHECK(ps_read32(base, kTaskAttr + 0x00) == 1);
    CHECK(ps_read32(base, kTaskAttr + 0x04) == 0x330000);
    CHECK(ps_read64(base, kTaskAttr + 0x08) == kElf);
    CHECK(ps_read64(base, kTaskAttr + 0x10) == kContext);
    CHECK(ps_read32(base, kTaskAttr + 0x18) == 0x2400);
    CHECK(ps_read32(base, kTaskAttr + 0x1C) == 0);
    CHECK(ps_read32(base, kTaskAttr + 0x20) == 0x03C00000u);
    CHECK(std::memcmp(base + kTaskAttr + 0x30, base + kArgs, 16) == 0);
    CHECK(ps_read64(base, kTaskAttr + 0x40) == 0); // exit code container
    CHECK(ps_read8(base, kTaskAttr + 0xFF) == 0);
    REQUIRE(a.call("cellSpursCreateTaskWithAttribute", kTs, kId, kTaskAttr) == 0);
    CHECK(ps_read32(base, kId) == 3);
    const u32 info3 = kTs + 0x80 + 3 * 0x30;
    CHECK(std::memcmp(base + info3, base + kArgs, 16) == 0);
    CHECK(ps_read64(base, info3 + 0x10) == kElf);
    CHECK(ps_read64(base, info3 + 0x18) == (u64{kContext} | 4));
    CHECK(ps_read32(base, info3 + 0x20) == 0x03C00000u);

    // No save config and no argument: no context, zero pattern and argument. 8-byte alignment is enough.
    REQUIRE(a.call("_cellSpursTaskAttributeInitialize", kTaskAttr + 8, 1, 0x330000, kElf, 0, 0) == 0);
    CHECK(ps_read64(base, kTaskAttr + 8 + 0x10) == 0);
    CHECK(ps_read64(base, kTaskAttr + 8 + 0x20) == 0);
    CHECK(ps_read64(base, kTaskAttr + 8 + 0x30) == 0);
    CHECK(a.call("_cellSpursTaskAttributeInitialize", kTaskAttr + 4, 1, 0x330000, kElf, 0, 0) == kTaskAlign);
    CHECK(a.call("_cellSpursTaskAttributeInitialize", 0, 1, 0x330000, kElf, 0, 0) == kTaskNull);
    CHECK(a.call("_cellSpursTaskAttributeInitialize", kTaskAttr, 1, 0x330000, 0, 0, 0) == kTaskNull);
    ps_write32(base, kSaveConfig + 0, 0); // a save config without a context: NULL, before the alignment check
    CHECK(a.call("_cellSpursTaskAttributeInitialize", kTaskAttr + 4, 1, 0x330000, kElf, kSaveConfig, 0) == kTaskNull);
    ps_write32(base, kSaveConfig + 0, kContext);
    ps_write32(base, kSaveConfig + 4, 0x1C00); // 3 blocks < 4
    CHECK(a.call("_cellSpursTaskAttributeInitialize", kTaskAttr, 1, 0x330000, kElf, kSaveConfig, 0) == kTaskInval);
    ps_write32(base, kSaveConfig + 4, 0x2400);
    CHECK(a.call("_cellSpursTaskAttributeInitialize", kTaskAttr, 1, 0x330000, kElf2, kSaveConfig, 0) == kTaskNoexec);

    // cellSpursCreateTaskWithAttribute's own checks, then an exit code container (CellSpursTaskset2 only: PERM).
    REQUIRE(a.call("_cellSpursTaskAttributeInitialize", kTaskAttr, 1, 0x330000, kElf, 0, 0) == 0);
    CHECK(a.call("cellSpursCreateTaskWithAttribute", kTs, 0, kTaskAttr) == kTaskNull);
    CHECK(a.call("cellSpursCreateTaskWithAttribute", 0, kId, kTaskAttr) == kTaskNull);
    CHECK(a.call("cellSpursCreateTaskWithAttribute", kTs + 0x40, kId, kTaskAttr) == kTaskAlign);
    CHECK(a.call("cellSpursCreateTaskWithAttribute", kTs, kId, 0) == kTaskNull);
    CHECK(a.call("cellSpursCreateTaskWithAttribute", kTs, kId, kTaskAttr + 4) == kTaskAlign);
    ps_write64(base, kTaskAttr + 0x40, kData + 0xC000);
    CHECK(a.call("cellSpursCreateTaskWithAttribute", kTs, kId, kTaskAttr) == kTaskPerm);
    ps_write64(base, kTaskAttr + 0x40, 0);
    ps_write32(base, kTaskAttr, 2);
    CHECK(a.call("cellSpursCreateTaskWithAttribute", kTs, kId, kTaskAttr) == kTaskInval);
    CHECK(ps_read32(base, kId) == 3); // written only on success

    // Run 013 (regression): GTA V's Bink called _cellSpursTaskAttributeInitialize with a stack save config in r7, no
    // argument in r8 and leftovers in r9/r10 (0x1049939f, 0x40), which pspocket used to read as an LS pattern and an
    // argument and refused. Pointers libsre would read and that are not guest memory are refused with
    // TASK_ERROR_FAULT and a log of the call site, never a host crash.
    constexpr u32 kCaller = kData + 0xA000;
    for (u32 i = 0; i < 16; i++) ps_write32(base, kCaller + 4 * i, 0x60000000); // nop: the caller's code before LR
    ps_write32(base, kCaller + 0x3C, 0x48000001); // bl
    a.ctx.lr = kCaller + 0x40;
    REQUIRE(a.call("_cellSpursTaskAttributeInitialize", kTaskAttr, 1, 0x330000, kElf, kSaveConfig, 0, 0x1049939f, 0x40) == 0);
    CHECK(ps_read64(base, kTaskAttr + 0x10) == kContext);
    CHECK(ps_read64(base, kTaskAttr + 0x30) == 0);
    CHECK(a.call("_cellSpursTaskAttributeInitialize", kTaskAttr, 1, 0x330000, kElf, 0x40, 0) == kTaskFault);
    CHECK(a.call("_cellSpursTaskAttributeInitialize", kTaskAttr, 1, 0x330000, kElf, 0, 0x40) == kTaskFault);
    ps_write32(base, kSaveConfig + 8, 0x40);
    CHECK(a.call("_cellSpursTaskAttributeInitialize", kTaskAttr, 1, 0x330000, kElf, kSaveConfig, 0) == kTaskFault);
    REQUIRE(a.call("cellSpursCreateTask", kTs, kId, kElf, 0, 0, 0x40, kArgs) == 0); // pattern ignored: no context
    CHECK(ps_read32(base, kId) == 4);
    CHECK(ps_read64(base, kTs + 0x80 + 4 * 0x30 + 0x20) == 0);
    CHECK(a.call("cellSpursCreateTask", kTs, kId, kElf, 0, 0, 0, 0x40) == kTaskFault);
    CHECK(a.call("cellSpursCreateTask", kTs, kId, 0x40, 0, 0, 0, 0) == kTaskFault);
    CHECK(ps_read32(base, kTs + 0x30) == 0xF8000000u); // refused calls took no ID

    // All 128 IDs taken.
    for (u32 w = 0; w < 4; w++) ps_write32(base, kTs + 0x30 + 4 * w, 0xFFFFFFFFu);
    CHECK(a.call("cellSpursCreateTask", kTs, kId, kElf, 0, 0, 0, 0) == kTaskAgain);
    // A task whose SPURS instance is in exception state is created, but its start reports TASK_ERROR_STAT.
    ps_write32(base, kTs + 0x30, 0x7FFFFFFFu);
    ps_write32(base, kSpurs + 0xD6C, 1);
    CHECK(a.call("cellSpursCreateTask", kTs, kId, kElf, 0, 0, 0, 0) == 0xFFFFFFFF8041090Full);
    CHECK(ps_read32(base, kTs + 0x30) == 0xFFFFFFFFu);
    ps_write32(base, kSpurs + 0xD6C, 0);
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

TEST_CASE("cellSheap as libsre: header and bitmap in guest memory, buddy allocation, free and merge", "[hle][spurs]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kHeap = kData + 0x10000;
    constexpr u64 kInval = 0xFFFFFFFF80410302ull, kAlign = 0xFFFFFFFF80410310ull;

    CHECK(a.call("cellSheapInitialize", 0, 0x80000, 0) == kInval);
    CHECK(a.call("cellSheapInitialize", kHeap + 0x40, 0x80000, 0) == kAlign);
    CHECK(a.call("cellSheapInitialize", kHeap, 0x7FF, 0) == kInval);
    CHECK(a.call("cellSheapInitialize", kHeap, 0x80000, 32) == kInval); // DMA tags are 0..31
    CHECK(a.call("cellSheapInitialize", kHeap, (u64{0x80} << 32) + 1, 0) == kInval);

    // GTA V's heap (run 008): 512 KB, tag 0. 0x7FF80 bytes after the header: a 0x800-byte bitmap, then 0x7F780 bytes of
    // blocks (0xFEF leaves) in a tree spanning 0x80000. Nodes 0x1FEF and 0x1FF (leaves 0xFEF..0xFFF) are past the end.
    std::memset(base + kHeap, 0xEE, 0x1000);
    REQUIRE(a.call("cellSheapInitialize", kHeap, 0x80000, 5) == 0);
    CHECK(ps_read32(base, kHeap + 0x00) == 0);           // lock
    CHECK(ps_read64(base, kHeap + 0x08) == kHeap);       // the header's own address
    CHECK(ps_read64(base, kHeap + 0x10) == kHeap + 0x80); // bitmap
    CHECK(ps_read64(base, kHeap + 0x18) == 0x800);       // bitmap size
    CHECK(ps_read32(base, kHeap + 0x20) == 0x1FEE);      // node count
    CHECK(ps_read32(base, kHeap + 0x24) == 0xEEEEEEEEu); // untouched
    CHECK(ps_read64(base, kHeap + 0x28) == kHeap + 0x880); // first block
    CHECK(ps_read64(base, kHeap + 0x30) == 0x80000);     // span
    CHECK(ps_read64(base, kHeap + 0x38) == 0x7F780);     // usable
    CHECK(ps_read32(base, kHeap + 0x40) == 5);           // DMA tag
    CHECK(ps_read32(base, kHeap + 0x44) == 5);
    CHECK(ps_read32(base, kHeap + 0x48) == 0xEEEEEEEEu);
    auto node = [&](u32 i) { return (ps_read32(base, kHeap + 0x80 + (i >> 4) * 4) >> (30 - 2 * (i & 15))) & 3; };
    CHECK(node(1) == 2);      // split: the padding is allocated below it
    CHECK(node(0xFF) == 2);
    CHECK(node(0x1FF) == 1);  // leaves 0xFF0..0xFFF
    CHECK(node(0x1FEF) == 1); // leaf 0xFEF
    CHECK(node(0xFF7) == 2);
    CHECK(node(0x2) == 0);
    CHECK(base[kHeap + 0x880] == 0xEE); // blocks are not touched

    // 128-byte leaves are depth 12 of the 0x80000 span; libsre hands out the leftmost free node of the depth.
    const u32 first = kHeap + 0x880;
    CHECK(a.call("cellSheapAllocate", kHeap, 1) == first);            // leaf 0x1000
    CHECK(a.call("cellSheapAllocate", kHeap, 0) == first + 0x80);     // 0 counts as 1: leaf 0x1001
    CHECK(a.call("cellSheapAllocate", kHeap, 0x81) == first + 0x100); // 256 bytes: node 0x801
    CHECK(a.call("cellSheapAllocate", kHeap, 0x80) == first + 0x200); // leaf 0x1004, past the full node 0x400
    CHECK(node(0x1000) == 1);
    CHECK(node(0x800) == 3); // both children used: full
    CHECK(node(0x801) == 1);
    CHECK(node(0x400) == 3);
    CHECK(node(0x200) == 2);
    // A half of the heap: the left one is split and the right one holds the padding (and is its depth's last node,
    // which libsre's search never takes). A quarter: node 5.
    CHECK(a.call("cellSheapAllocate", kHeap, 0x40000) == 0);
    CHECK(a.call("cellSheapAllocate", kHeap, 0x20000) == first + 0x20000);
    CHECK(a.call("cellSheapAllocate", kHeap, 0x80001) == 0);     // larger than the span
    CHECK(a.call("cellSheapAllocate", kHeap + 0x40, 0x80) == 0); // misaligned heap

    // Free: a block's start or any 128-byte step inside it frees the allocation; full ancestors become split, free
    // buddies merge.
    REQUIRE(a.call("cellSheapFree", kHeap, first + 0x180) == 0); // inside the 256-byte block
    CHECK(node(0x801) == 0);
    CHECK(node(0x800) == 3); // leaves 0x1000 and 0x1001 are still allocated
    CHECK(node(0x400) == 2);
    CHECK(a.call("cellSheapFree", kHeap, first + 0x40) == kInval); // not 128-byte aligned
    CHECK(a.call("cellSheapFree", kHeap, first - 0x80) == kInval); // below the first block
    CHECK(a.call("cellSheapFree", kHeap + 0x40, first) == kAlign);
    REQUIRE(a.call("cellSheapFree", kHeap, first) == 0);
    CHECK(node(0x800) == 2);
    REQUIRE(a.call("cellSheapFree", kHeap, first + 0x80) == 0);
    CHECK(node(0x800) == 0); // merged up to node 0x400, whose buddy 0x401 is split
    CHECK(node(0x400) == 0);
    REQUIRE(a.call("cellSheapFree", kHeap, first + 0x200) == 0);
    CHECK(node(4) == 0); // merged up to the first quarter; node 5 is allocated
    CHECK(node(2) == 2);
    REQUIRE(a.call("cellSheapFree", kHeap, first + 0x20000) == 0);
    CHECK(node(5) == 0);
    CHECK(node(2) == 0);
    CHECK(node(1) == 2);
    CHECK(a.call("cellSheapAllocate", kHeap, 0x40000) == first); // the left half, whole
    CHECK(node(2) == 1);
    REQUIRE(a.call("cellSheapFree", kHeap, first + 0x3FF80) == 0);
    CHECK(node(2) == 0);
    CHECK(node(0x1FF) == 1); // the padding stays allocated
}

TEST_CASE("sys_fs (cellFs): open, write, read, offsets, seek, stat, directories, allocate", "[hle][fs]")
{
    namespace fsys = std::filesystem;
    const fsys::path root = fsys::temp_directory_path() / ("pspocket-hle-fs-" + std::to_string(::getpid()));
    fsys::remove_all(root);
    fsys::create_directories(root / "game" / "TEST00000");
    Fixture f;
    u8* base = f.base();
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", root.string(), false));
    GuestThread a(f, 0, "a");
    constexpr u32 kPath = kData + 0x1000, kFd = kData + 0x1100, kBuf = kData + 0x2000, kCount = kData + 0x1108,
                  kPos = kData + 0x1110, kStat = kData + 0x1200, kDir = kData + 0x3000, kPath2 = kData + 0x1400;
    constexpr s32 O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_CREAT = 0100;
    const auto put = [&](u32 addr, std::string_view s) {
        std::memcpy(base + addr, s.data(), s.size());
        base[addr + s.size()] = 0;
    };

    put(kPath, "/dev_hdd0/game/TEST00000/save.bin");
    REQUIRE(a.call("cellFsOpen", kPath, O_RDWR | O_CREAT, kFd, 0, 0) == 0);
    const u32 fd = ps_read32(base, kFd);
    CHECK(fd == 3);
    put(kBuf, "hello, cellFs");
    REQUIRE(a.call("cellFsWrite", fd, kBuf, 13, kCount) == 0);
    CHECK(ps_read64(base, kCount) == 13);
    REQUIRE(a.call("cellFsWrite", fd, kBuf, 2, 0) == 0); // no count pointer: a stack variable takes it

    // Read at an offset without moving the position (sys_fs_fcntl 0x8000000A).
    std::memset(base + kBuf, 0, 32);
    REQUIRE(a.call("cellFsReadWithOffset", fd, 7, kBuf, 6, kCount) == 0);
    CHECK(std::string(reinterpret_cast<const char*>(base + kBuf), 6) == "cellFs");
    CHECK(ps_read64(base, kCount) == 6);
    REQUIRE(a.call("cellFsLseek", fd, 0, 1 /* CUR */, kPos) == 0);
    CHECK(ps_read64(base, kPos) == 15); // unchanged by the offset read
    put(kBuf, "HELLO");
    REQUIRE(a.call("cellFsWriteWithOffset", fd, 0, kBuf, 5, kCount) == 0);
    CHECK(ps_read64(base, kCount) == 5);
    REQUIRE(a.call("cellFsLseek", fd, 0, 0 /* SET */, kPos) == 0);
    std::memset(base + kBuf, 0, 32);
    REQUIRE(a.call("cellFsRead", fd, kBuf, 100, kCount) == 0);
    CHECK(ps_read64(base, kCount) == 15);
    CHECK(std::string(reinterpret_cast<const char*>(base + kBuf), 15) == "HELLO, cellFshe");
    CHECK(a.call("cellFsLseek", fd, 0, 0, 0) == r3_of(lv2::CELL_EFAULT));
    REQUIRE(a.call("cellFsFsync", fd) == 0);
    REQUIRE(a.call("cellFsClose", fd) == 0);
    CHECK(a.call("cellFsClose", fd) == r3_of(lv2::CELL_EBADF));
    CHECK(a.call("cellFsReadWithOffset", 300, 0, kBuf, 4, kCount) == r3_of(lv2::CELL_EBADF));
    CHECK(ps_read64(base, kCount) == 0);

    // stat, allocate (sets the size), utime, chmod.
    REQUIRE(a.call("cellFsStat", kPath, kStat) == 0);
    CHECK(ps_read64(base, kStat + 36) == 15);
    REQUIRE(a.call("cellFsAllocateFileAreaWithoutZeroFill", kPath, 0x1000) == 0);
    REQUIRE(a.call("cellFsStat", kPath, kStat) == 0);
    CHECK(ps_read64(base, kStat + 36) == 0x1000);
    ps_write64(base, kBuf, 1000000);
    ps_write64(base, kBuf + 8, 2000000);
    REQUIRE(a.call("cellFsUtime", kPath, kBuf) == 0);
    CHECK(fsys::last_write_time(root / "game" / "TEST00000" / "save.bin").time_since_epoch().count() != 0);
    REQUIRE(a.call("cellFsChmod", kPath, 0666) == 0);
    CHECK(a.call("cellFsStat", kPath, 0) == r3_of(lv2::CELL_EFAULT));
    put(kPath2, "/dev_hdd0/game/TEST00000/none.bin");
    CHECK(a.call("cellFsStat", kPath2, kStat) == r3_of(lv2::CELL_ENOENT));
    CHECK(a.call("cellFsOpen", kPath2, O_RDONLY, kFd, 0, 0) == r3_of(lv2::CELL_ENOENT));
    CHECK(a.call("cellFsOpen", kPath2, O_WRONLY, 0, 0, 0) == r3_of(lv2::CELL_EFAULT));

    // Directories, rename, unlink.
    put(kPath2, "/dev_hdd0/game/TEST00000/dir");
    REQUIRE(a.call("cellFsMkdir", kPath2, 0777) == 0);
    REQUIRE(a.call("cellFsOpendir", kPath2, kFd) == 0);
    const u32 dfd = ps_read32(base, kFd);
    REQUIRE(a.call("cellFsReaddir", dfd, kDir, kCount) == 0);
    CHECK(base[kDir + 2] == '.');
    CHECK(a.call("cellFsReaddir", dfd, 0, kCount) == r3_of(lv2::CELL_EFAULT));
    REQUIRE(a.call("cellFsClosedir", dfd) == 0);
    REQUIRE(a.call("cellFsRmdir", kPath2) == 0);
    put(kPath2, "/dev_hdd0/game/TEST00000/moved.bin");
    REQUIRE(a.call("cellFsRename", kPath, kPath2) == 0);
    CHECK(fsys::exists(root / "game" / "TEST00000" / "moved.bin"));
    REQUIRE(a.call("cellFsOpen", kPath2, O_RDWR, kFd, 0, 0) == 0);
    REQUIRE(a.call("cellFsFtruncate", ps_read32(base, kFd), 3) == 0);
    REQUIRE(a.call("cellFsClose", ps_read32(base, kFd)) == 0);
    CHECK(fsys::file_size(root / "game" / "TEST00000" / "moved.bin") == 3);
    REQUIRE(a.call("cellFsUnlink", kPath2) == 0);
    CHECK_FALSE(fsys::exists(root / "game" / "TEST00000" / "moved.bin"));
    fsys::remove_all(root);
}

// A PARAM.SFO with string entries (key, value, max size), written the way the PS3 lays it out.
static std::vector<u8> make_param_sfo(const std::vector<std::tuple<std::string, std::string, u32>>& entries)
{
    std::vector<u8> keys, data, index;
    const auto put32 = [](std::vector<u8>& v, u32 x) {
        for (int i = 0; i < 4; i++) v.push_back(static_cast<u8>(x >> (8 * i)));
    };
    for (const auto& [key, value, max] : entries) { // keys must be sorted
        index.push_back(static_cast<u8>(keys.size()));
        index.push_back(static_cast<u8>(keys.size() >> 8));
        index.push_back(0x04);
        index.push_back(0x02);
        put32(index, static_cast<u32>(value.size() + 1));
        put32(index, max);
        put32(index, static_cast<u32>(data.size()));
        keys.insert(keys.end(), key.begin(), key.end());
        keys.push_back(0);
        const usize at = data.size();
        data.insert(data.end(), value.begin(), value.end());
        data.resize(at + max, 0);
    }
    while (keys.size() % 4) keys.push_back(0);
    std::vector<u8> out;
    put32(out, 0x46535000);
    put32(out, 0x101);
    put32(out, static_cast<u32>(20 + index.size()));
    put32(out, static_cast<u32>(20 + index.size() + keys.size()));
    put32(out, static_cast<u32>(entries.size()));
    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), keys.begin(), keys.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

TEST_CASE("cellGame: disc boot check and permit, game data created then found; sceNp2Init", "[hle][game]")
{
    namespace fsys = std::filesystem;
    const fsys::path root = fsys::temp_directory_path() / ("pspocket-hle-game-" + std::to_string(::getpid()));
    fsys::remove_all(root);
    fsys::create_directories(root / "game");
    hle::ProcessConfig config;
    config.game.title_id = "BLES01807";
    config.game.param_sfo = make_param_sfo({{"APP_VER", "01.00", 8}, {"CATEGORY", "DG", 4},
        {"TITLE", "Test Game", 128}, {"TITLE_ID", "BLES01807", 16}, {"VERSION", "01.00", 8}});
    Fixture f(config);
    u8* base = f.base();
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", root.string(), false));
    GuestThread a(f, 0, "a");
    constexpr u32 kType = kData + 0x100, kAttributes = kData + 0x104, kSize = kData + 0x110, kDirName = kData + 0x120,
                  kContent = kData + 0x200, kUsrdir = kData + 0x300, kBuf = kData + 0x400, kInit = kData + 0x500,
                  kName = kData + 0x600;
    constexpr u64 kBusy = 0xFFFFFFFF8002CB23ull, kFailure = 0xFFFFFFFF8002CB22ull, kParam = 0xFFFFFFFF8002CB07ull,
                  kNotSupported = 0xFFFFFFFF8002CB21ull, kExist = 0xFFFFFFFF8002CB26ull, kBroken = 0xFFFFFFFF8002CB05ull,
                  kInvalidId = 0xFFFFFFFF8002CB25ull;
    const auto str = [&](u32 addr) { return std::string(reinterpret_cast<const char*>(base + addr)); };
    const auto put = [&](u32 addr, std::string_view v) {
        std::memcpy(base + addr, v.data(), v.size());
        base[addr + v.size()] = 0;
    };

    // The disc game: boot check, then its content directories.
    REQUIRE(a.call("cellGameBootCheck", kType, kAttributes, kSize, kDirName) == 0);
    CHECK(ps_read32(base, kType) == 1); // CELL_GAME_GAMETYPE_DISC
    CHECK(ps_read32(base, kAttributes) == 0);
    CHECK(static_cast<s32>(ps_read32(base, kSize + 4)) == -1); // size not calculated
    CHECK(ps_read32(base, kSize + 8) == 4);
    CHECK(a.call("cellGameBootCheck", kType, kAttributes, kSize, kDirName) == kBusy);
    REQUIRE(a.call("cellGameGetParamString", 100, kBuf, 16) == 0); // TITLE_ID
    CHECK(str(kBuf) == "BLES01807");
    CHECK(a.call("cellGameGetParamString", 101, kBuf, 16) == kNotSupported); // VERSION: game data only
    CHECK(a.call("cellGameGetParamString", 999, kBuf, 16) == kInvalidId);
    REQUIRE(a.call("cellGameContentPermit", kContent, kUsrdir) == 0);
    CHECK(str(kContent) == "/dev_bdvd/PS3_GAME");
    CHECK(str(kUsrdir) == "/dev_bdvd/PS3_GAME/USRDIR");
    CHECK(a.call("cellGameContentPermit", kContent, kUsrdir) == kFailure); // the session is closed
    CHECK(a.call("cellGameGetParamString", 100, kBuf, 16) == kFailure);
    REQUIRE(a.call("cellGameDataCheck", 1, 0, kSize) == 0); // DISC: the disc's PARAM.SFO (category DG)
    REQUIRE(a.call("cellGameContentPermit", kContent, kUsrdir) == 0);

    // Game data that doesn't exist yet: RET_NONE, then create it.
    put(kName, "BLES01807DATA");
    REQUIRE(a.call("cellGameDataCheck", 3, kName, kSize) == 2); // CELL_GAME_RET_NONE
    CHECK(ps_read32(base, kSize + 4) == 0);
    std::memset(base + kInit, 0, 0x100);
    put(kInit, "Test Data");
    put(kInit + 128, "BLES01807");
    put(kInit + 140, "01.00");
    REQUIRE(a.call("cellGameCreateGameData", kInit, kContent, kUsrdir) == 0);
    CHECK(str(kContent).starts_with("/dev_hdd0/game/_GDATA_"));
    CHECK(str(kUsrdir) == str(kContent) + "/USRDIR");
    REQUIRE(a.call("cellGameGetParamString", 0, kBuf, 128) == 0); // TITLE
    CHECK(str(kBuf) == "Test Data");
    REQUIRE(a.call("cellGameContentPermit", kContent, kUsrdir) == 0);
    CHECK(str(kContent) == "/dev_hdd0/game/BLES01807DATA");
    CHECK(str(kUsrdir) == "/dev_hdd0/game/BLES01807DATA/USRDIR");
    CHECK(fsys::is_directory(root / "game" / "BLES01807DATA" / "USRDIR"));
    CHECK(fsys::exists(root / "game" / "BLES01807DATA" / "PARAM.SFO"));

    // Found next time, with the PARAM.SFO written above.
    REQUIRE(a.call("cellGameDataCheck", 3, kName, kSize) == 0);
    CHECK(static_cast<s32>(ps_read32(base, kSize + 4)) == -1);
    REQUIRE(a.call("cellGameGetParamString", 100, kBuf, 16) == 0);
    CHECK(str(kBuf) == "BLES01807");
    REQUIRE(a.call("cellGameGetParamString", 101, kBuf, 3) == 0); // truncated to the buffer
    CHECK(str(kBuf) == "01");
    CHECK(a.call("cellGameCreateGameData", kInit, kContent, kUsrdir) == kExist);
    REQUIRE(a.call("cellGameContentPermit", kContent, kUsrdir) == 0);
    CHECK(str(kContent) == "/dev_hdd0/game/BLES01807DATA");

    // A corrupt PARAM.SFO is BROKEN, and the session stays closed.
    fsys::create_directories(root / "game" / "BROKEN");
    std::ofstream(root / "game" / "BROKEN" / "PARAM.SFO") << "not a psf";
    put(kName, "BROKEN");
    CHECK(a.call("cellGameDataCheck", 3, kName, kSize) == kBroken);
    CHECK(a.call("cellGameDataCheck", 4, kName, kSize) == kParam);
    CHECK(a.call("cellGameDataCheck", 3, 0, kSize) == kParam);
    CHECK(a.call("cellGameContentErrorDialog", 0, 0, kName) == 0);
    CHECK(a.call("cellGameContentErrorDialog", 5, 0, 0) == kParam);

    // sceNp2Init brings NP up (offline).
    constexpr u32 kPool = kData + 0x40000, kStatus = kData + 0x700;
    CHECK(a.call("sceNpManagerGetStatus", kStatus) == 0xFFFFFFFF8002AA01ull); // not initialized
    CHECK(a.call("sceNp2Init", 0x1000, kPool) == 0xFFFFFFFF8002AA0Full);      // pool below 128 KB
    CHECK(a.call("sceNp2Init", 0x20000, 0) == 0xFFFFFFFF8002AA03ull);
    REQUIRE(a.call("sceNp2Init", 0x20000, kPool) == 0);
    CHECK(a.call("sceNp2Init", 0x20000, kPool) == 0xFFFFFFFF8002AA02ull);
    REQUIRE(a.call("sceNpManagerGetStatus", kStatus) == 0);
    CHECK(static_cast<s32>(ps_read32(base, kStatus)) == -1); // offline
    // The manager callback is kept (never called offline): a null one is refused.
    CHECK(a.call("sceNpManagerRegisterCallback", 0, kStatus) == 0xFFFFFFFF8002AA03ull);
    CHECK(a.call("sceNpManagerRegisterCallback", kParam, kStatus) == 0);
    CHECK(a.call("sceNpManagerUnregisterCallback") == 0);
    // One basic event handler (never called offline either).
    CHECK(a.call("sceNpBasicUnregisterHandler") == 0xFFFFFFFF8002A66Dull); // not registered
    CHECK(a.call("sceNpBasicRegisterContextSensitiveHandler", kName, 0, 0) == 0xFFFFFFFF8002A665ull);
    CHECK(a.call("sceNpBasicRegisterContextSensitiveHandler", kName, kParam, 0) == 0);
    CHECK(a.call("sceNpBasicRegisterContextSensitiveHandler", kName, kParam, 0) == 0xFFFFFFFF8002A66Bull);
    CHECK(a.call("sceNpBasicUnregisterHandler") == 0);
    REQUIRE(a.call("sceNp2Term") == 0);
    CHECK(a.call("sceNpManagerRegisterCallback", kParam, kStatus) == 0xFFFFFFFF8002AA01ull); // not initialized
    CHECK(a.call("sceNpManagerUnregisterCallback") == 0xFFFFFFFF8002AA01ull);
    CHECK(a.call("sceNp2Term") == 0xFFFFFFFF8002AA01ull);
    fsys::remove_all(root);
}

TEST_CASE("cellGame: param ints and strings, sizes, patch check, delete, themes; cellSysutil's game rest (D-183)",
    "[hle][game]")
{
    namespace fsys = std::filesystem;
    const fsys::path root = fsys::temp_directory_path() / ("pspocket-hle-game2-" + std::to_string(::getpid()));
    fsys::remove_all(root);
    fsys::create_directories(root / "hdd0" / "game");
    fsys::create_directories(root / "bdvd" / "PS3_GAME" / "USRDIR");
    std::ofstream(root / "bdvd" / "PS3_GAME" / "USRDIR" / "EBOOT.BIN") << std::string(3000, 'e');
    hle::ProcessConfig config;
    config.game.title_id = "BLES01807";
    config.game.param_sfo = make_param_sfo({{"APP_VER", "01.00", 8}, {"CATEGORY", "DG", 4},
        {"TITLE", "Test Game", 128}, {"TITLE_ID", "BLES01807", 16}, {"VERSION", "01.00", 8}});
    Fixture f(config);
    u8* base = f.base();
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", (root / "hdd0").string(), false));
    GuestThread a(f, 0, "a");
    constexpr u32 kSize = kData + 0x110, kContent = kData + 0x200, kUsrdir = kData + 0x300, kBuf = kData + 0x400,
                  kInit = kData + 0x500, kName = kData + 0x600, kOut = kData + 0x700, kTheme = kData + 0x10000;
    constexpr u64 kFailure = 0xFFFFFFFF8002CB22ull, kParam = 0xFFFFFFFF8002CB07ull,
                  kNotSupported = 0xFFFFFFFF8002CB21ull, kInvalidId = 0xFFFFFFFF8002CB25ull,
                  kNotFound = 0xFFFFFFFF8002CB04ull, kNotPatch = 0xFFFFFFFF8002CB27ull,
                  kBadTheme = 0xFFFFFFFF8002CB28ull, kHddParam = 0xFFFFFFFF8002BA04ull,
                  kHddFailure = 0xFFFFFFFF8002BA07ull, kDataParam = 0xFFFFFFFF8002B604ull;
    const auto put = [&](u32 addr, std::string_view v) {
        std::memcpy(base + addr, v.data(), v.size());
        base[addr + v.size()] = 0;
    };

    // No session: FAILURE; the disc's session can't set strings and has no integers.
    CHECK(a.call("cellGameGetParamInt", 102, kOut) == kFailure);
    CHECK(a.call("cellGameGetSizeKB", kOut) == kFailure);
    CHECK(a.call("cellGameGetSizeKB", 0) == kParam);
    REQUIRE(a.call("cellGameDataCheck", 1, 0, kSize) == 0);
    ps_write32(base, kOut, 7);
    REQUIRE(a.call("cellGameGetParamInt", 102, kOut) == 0); // PARENTAL_LEVEL: not in this PARAM.SFO
    CHECK(ps_read32(base, kOut) == 0);
    CHECK(a.call("cellGameGetParamInt", 105, kOut) == kInvalidId);
    CHECK(a.call("cellGameGetParamInt", 102, 0) == kParam);
    put(kBuf, "New Title");
    CHECK(a.call("cellGameSetParamString", 0, kBuf) == kNotSupported);
    REQUIRE(a.call("cellGameContentPermit", kContent, kUsrdir) == 0);
    CHECK(a.call("cellGameSetParamString", 0, kBuf) == kFailure);

    // Game data: created, its title set, found with that title, sized, then deleted.
    put(kName, "BLES01807DATA");
    REQUIRE(a.call("cellGameDataCheck", 3, kName, kSize) == 2);
    std::memset(base + kInit, 0, 0x100);
    put(kInit, "Test Data");
    put(kInit + 128, "BLES01807");
    put(kInit + 140, "01.00");
    REQUIRE(a.call("cellGameCreateGameData", kInit, kContent, kUsrdir) == 0);
    REQUIRE(a.call("cellGameSetParamString", 3, kBuf) == 0);           // TITLE_ENGLISH (TITLE_01)
    REQUIRE(a.call("cellGameSetParamString", 0, kBuf) == 0);           // TITLE
    CHECK(a.call("cellGameSetParamString", 100, kBuf) == kNotSupported); // TITLE_ID can't be set
    CHECK(a.call("cellGameSetParamString", 999, kBuf) == kInvalidId);
    REQUIRE(a.call("cellGameContentPermit", kContent, kUsrdir) == 0);
    std::ofstream(root / "hdd0" / "game" / "BLES01807DATA" / "USRDIR" / "DATA.BIN") << std::string(1500, 'd');
    REQUIRE(a.call("cellGameDataCheck", 3, kName, kSize) == 0);
    REQUIRE(a.call("cellGameGetParamString", 0, kBuf, 128) == 0);
    CHECK(std::string(reinterpret_cast<const char*>(base + kBuf)) == "New Title");
    REQUIRE(a.call("cellGameGetSizeKB", kOut) == 0);
    const u64 sfo_kb = (fsys::file_size(root / "hdd0" / "game" / "BLES01807DATA" / "PARAM.SFO") + 1023) / 1024;
    CHECK(ps_read32(base, kOut) == 2 + sfo_kb);
    CHECK(a.call("cellGameDeleteGameData", kName) == kNotSupported); // its session may write it (RPCS3)
    REQUIRE(a.call("cellGameContentPermit", kContent, kUsrdir) == 0);
    CHECK(a.call("cellGameDeleteGameData", kName) == 0);
    CHECK_FALSE(fsys::exists(root / "hdd0" / "game" / "BLES01807DATA"));
    CHECK(a.call("cellGameDeleteGameData", kName) == kNotFound);
    put(kName, "bad");
    CHECK(a.call("cellGameDeleteGameData", kName) == kParam);
    fsys::create_directories(root / "hdd0" / "game" / "OTHER");
    std::ofstream(root / "hdd0" / "game" / "OTHER" / "PARAM.SFO") << "not a psf";
    put(kName, "OTHER");
    CHECK(a.call("cellGameDeleteGameData", kName) == kNotSupported);

    // A disc game has no patch; the paths RPCS3 leaves empty.
    CHECK(a.call("cellGamePatchCheck", kSize, 0) == kNotPatch);
    CHECK(a.call("cellGameGetDiscContentInfoUpdatePath", 0) == kParam);
    CHECK(a.call("cellGameGetDiscContentInfoUpdatePath", kBuf) == 0);
    CHECK(a.call("cellGameGetLocalWebContentPath", kBuf) == 0);

    // A theme from a buffer goes to /dev_hdd0/theme, once; a file that isn't one is refused.
    std::memset(base + kTheme, 0x5A, 4096);
    std::memcpy(base + kTheme, "P3TF", 4);
    REQUIRE(a.call("cellGameThemeInstallFromBuffer", 4096, 4096, kTheme, 0, 0) == 0);
    REQUIRE(fsys::is_directory(root / "hdd0" / "theme"));
    usize themes = 0;
    for (const auto& e : fsys::directory_iterator(root / "hdd0" / "theme")) {
        CHECK(e.path().filename().string().starts_with("BLES01807_"));
        CHECK(fsys::file_size(e.path()) == 4096);
        themes++;
    }
    CHECK(themes == 1);
    CHECK(a.call("cellGameThemeInstallFromBuffer", 4096, 4096, kTheme, 0, 0) == 0); // already there
    std::memcpy(base + kTheme, "XXXX", 4);
    CHECK(a.call("cellGameThemeInstallFromBuffer", 4096, 4096, kTheme, 0, 0) == kBadTheme);
    CHECK(a.call("cellGameThemeInstallFromBuffer", 4096, 4095, kTheme, 0, 0) == kParam); // buffer below 4 KB
    CHECK(a.call("cellGameThemeInstallFromBuffer", 8192, 4096, kTheme, 0, 0) == kParam); // larger, no callback

    // cellSysutil: the booted directory's size (the disc mounted on a host directory), the system versions.
    CHECK(a.call("cellHddGameGetSizeKB", kOut) == kHddFailure); // no disc mounted
    REQUIRE(f.kernel->vfs_mount("/dev_bdvd", (root / "bdvd").string(), true));
    REQUIRE(a.call("cellHddGameGetSizeKB", kOut) == 0);
    CHECK(ps_read32(base, kOut) == 3);
    CHECK(a.call("cellHddGameGetSizeKB", 0) == kHddParam);
    REQUIRE(a.call("cellGameDataGetSizeKB", kOut) == 0); // no check yet: the booted directory
    CHECK(ps_read32(base, kOut) == 3);
    put(kBuf, "04.8300");
    CHECK(a.call("cellHddGameSetSystemVer", kBuf) == 0);
    CHECK(a.call("cellGameDataSetSystemVer", kBuf) == 0);
    put(kBuf, "4.83");
    CHECK(a.call("cellHddGameSetSystemVer", kBuf) == kHddParam);
    CHECK(a.call("cellGameDataSetSystemVer", 0) == kDataParam);
    CHECK(a.call("cellGameRegisterDiscChangeCallback", kBuf, kBuf) == 0);
    CHECK(a.call("cellGameUnregisterDiscChangeCallback") == 0);
    CHECK(a.call("cellDiscGameUnregisterDiscChangeCallback") == 0);
    CHECK(a.call("cellGameDataExitBroken") == 0);
    CHECK(a.call("cellHddGameExitBroken") == 0);
    fsys::remove_all(root);
}

TEST_CASE("cellSysmodule: module files, images and the debug mode (RPCS3)", "[hle][sysmodule]")
{
    namespace fsys = std::filesystem;
    const fsys::path root = fsys::temp_directory_path() / ("pspocket-hle-flash-" + std::to_string(::getpid()));
    fsys::remove_all(root);
    fsys::create_directories(root / "sys" / "external");
    {
        std::ofstream out(root / "sys" / "external" / "flashATRAC.pic", std::ios::binary);
        for (int i = 0; i < 300; i++) out.put(static_cast<char>(i)); // synthetic bytes
    }
    {
        Fixture f;
        REQUIRE(f.kernel->vfs_mount("/dev_flash", root.string(), true));
        GuestThread g(f, 0, "sysmodule");
        u8* b = f.base();
        constexpr u64 kFatal = 0xFFFFFFFF800120FFull;
        constexpr u32 kSize = kData + 0x100, kUnk = kData + 0x108, kPath = kData + 0x200, kBuf = kData + 0x1000;

        // 0xFF00 is the Flash ATRAC picture; 0xFF01 has no file; others are FATAL.
        REQUIRE(g.call("cellSysmoduleGetImagesize", 0xFF00, kSize) == 0);
        CHECK(ps_read32(b, kSize) == 300);
        ps_write32(b, kSize, 0xAAAA);
        CHECK(g.call("cellSysmoduleGetImagesize", 0xFF01, kSize) == kFatal);
        CHECK(ps_read32(b, kSize) == 0);
        CHECK(g.call("cellSysmoduleGetImagesize", 0x0010, kSize) == kFatal);
        CHECK(g.call("cellSysmoduleGetImagesize", 0xFF02, kSize) == kFatal);

        ps_write32(b, kSize, 100); // at most 100 bytes
        REQUIRE(g.call("cellSysmoduleFetchImage", 0xFF00, kBuf, kSize) == 0);
        CHECK(ps_read32(b, kSize) == 100);
        CHECK(ps_read8(b, kBuf + 99) == 99);
        ps_write32(b, kSize, 1000); // more than there is
        REQUIRE(g.call("cellSysmoduleFetchImage", 0xFF00, kBuf, kSize) == 0);
        CHECK(ps_read32(b, kSize) == 300);
        CHECK(ps_read8(b, kBuf + 299) == static_cast<u8>(299));
        ps_write32(b, kSize, 1000);
        CHECK(g.call("cellSysmoduleFetchImage", 0xFF01, kBuf, kSize) == kFatal);
        CHECK(ps_read32(b, kSize) == 0);
        CHECK(g.call("cellSysmoduleFetchImage", 0x0001, kBuf, kSize) == kFatal);

        // A module file this process can't load (the tests have no module loader): FATAL, with the error as the ID.
        const char path[] = "/app_home/mod.sprx";
        std::memcpy(b + kPath, path, sizeof(path));
        REQUIRE(g.call("cellSysmoduleLoadModuleFile", kPath, kUnk) == kFatal);
        CHECK(static_cast<s32>(ps_read32(b, kUnk)) < 0);
        CHECK(ps_read32(b, kUnk + 4) == 0xFFFFFFFF);
        CHECK(g.call("cellSysmoduleUnloadModuleFile", kUnk) == kFatal);

        CHECK(g.call("cellSysmoduleSetDebugmode", 1) == 0);
        CHECK(g.call("cellSysmoduleSetDebugmode", 0) == 0);
    }
    fsys::remove_all(root);
}

TEST_CASE("liblv2's sysPs2Disc, sysBdMediaId and sysConsoleId: liblv2's sys_ss calls", "[hle]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    struct Seen {
        std::mutex m;
        std::vector<std::array<u64, 3>> calls;
    } seen;
    f.kernel->set_syscall_observer(
        [](void* user, u32 number, const u64* args) {
            if (number < 870 || number > 879) return;
            auto* s = static_cast<Seen*>(user);
            std::lock_guard lock(s->m);
            s->calls.push_back({number, args[0], args[1]});
        },
        &seen);
    constexpr u32 kBuf = kData + 0x200;
    CHECK(a.call("sysPs2Disc_BB7CD1AE", 7) == 0);
    CHECK(a.call("sysPs2Disc_A84FD3C3") == 0);
    CHECK(a.call("sys_get_bd_media_id", 0) == r3_of(lv2::CELL_EINVAL));
    CHECK(a.call("sys_get_bd_media_id", kBuf) == 0);
    CHECK(a.call("sys_get_console_id", 0) == r3_of(lv2::CELL_EINVAL));
    CHECK(a.call("sys_get_console_id", kBuf) == r3_of(lv2::CELL_ENOSYS)); // lv2 answers only a root process
    f.kernel->set_syscall_observer(nullptr, nullptr);
    // The null buffers never reach lv2.
    REQUIRE(seen.calls.size() == 4);
    CHECK(seen.calls[0] == std::array<u64, 3>{876, 0x20000, 7});
    CHECK(seen.calls[1] == std::array<u64, 3>{876, 0x20001, 1});
    CHECK(seen.calls[2] == std::array<u64, 3>{879, 0x10001, kBuf});
    CHECK(seen.calls[3][0] == 870);
    CHECK(seen.calls[3][1] == kBuf);
}
