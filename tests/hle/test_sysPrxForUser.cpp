// HLE functions on real lv2 threads: sysPrxForUser lwmutex (RPCS3 sys_lwmutex_.cpp semantics, including contention
// through the kernel's sleep queue), sys_ppu_thread_get_id and sys_time_get_system_time, cellSysmodule's load
// bookkeeping, and lv2's null_func syscall 988.

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/ppu/runtime.h"

using namespace pspocket;

namespace {

constexpr u32 kData = 0x00100000;   // scratch guest data
constexpr u32 kStacks = 0x00200000; // one 64 KB guest stack per test thread

u64 r3_of(lv2::CellError e) { return lv2::to_r3(lv2::err(e)); }

struct Fixture {
    GuestMemory mem;
    std::unique_ptr<lv2::Kernel> kernel;
    std::unique_ptr<hle::HleProcess> hle;

    Fixture()
    {
        REQUIRE(mem.reserve());
        kernel = std::make_unique<lv2::Kernel>(mem);
        kernel->install();
        hle = std::make_unique<hle::HleProcess>(*kernel, 0x10000, 0x10000000);
        hle->install();
        REQUIRE(hle->main_memory().reserve(kData, 0x200000));
        REQUIRE(mem.commit(kData, 0x200000));
    }
    ~Fixture()
    {
        hle.reset();
        kernel.reset();
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
        REQUIRE(fn != nullptr);
        const u64 v[] = {0, static_cast<u64>(args)...}; // leading 0: no zero-length array for calls without arguments
        for (usize i = 0; i < sizeof...(A); i++) ctx.gpr[3 + i] = v[i + 1];
        const u64 sp = ctx.gpr[1];
        fn->fn(ctx, f.base());
        CHECK(ctx.gpr[1] == sp); // stack variables released
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

u32 owner(u8* base) { return ps_read32(base, kLw); }
u32 waiters(u8* base) { return ps_read32(base, kLw + 4); }

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
    CHECK(a.call("cellSysmoduleIsLoaded", 0xF000) == kUnknown);
    CHECK(a.call("cellSysmoduleFinalize") == 0);
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
    CHECK(ps_read32(base, kSpurs + 0x60) == 0xFFFFFFFFu); // wklFlag.flag
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
