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
#include "pspocket/lv2/syscalls.hpp"
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

    // A taskset that was never created (workload 0 not prepared) can't be shut down.
    std::memset(base + kTs, 0, 0x80);
    ps_write64(base, kTs + 0x60, kSpurs);
    CHECK(a.call("cellSpursShutdownTaskset", kTs) == kTaskStat);
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
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);

    CHECK(a.call("sys_net_initialize_network_ex", kData) == 0);
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
