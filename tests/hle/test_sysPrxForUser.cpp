// sysPrxForUser HLE functions on real lv2 threads: lwmutex (RPCS3 sys_lwmutex_.cpp semantics, including contention
// through the kernel's sleep queue), sys_ppu_thread_get_id, and lv2's null_func syscall 988.

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
            if (x.module == "sysPrxForUser" && x.name == name) fn = &x;
        REQUIRE(fn != nullptr);
        const u64 v[] = {static_cast<u64>(args)...};
        for (usize i = 0; i < sizeof...(A); i++) ctx.gpr[3 + i] = v[i];
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
