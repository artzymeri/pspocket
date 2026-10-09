#pragma once

// Shared fixture for HLE tests: a reserved guest address space, an lv2 kernel and an HLE process, and host threads
// with an lv2 identity that call HLE functions the way guest code does (arguments in r3..r10, then the stack).
// Everything is synthetic (no game data). test_sysPrxForUser.cpp has its own copy of the same pattern.

#include <catch_amalgamated.hpp>

#include <atomic>
#include <exception>
#include <memory>
#include <string_view>
#include <thread>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/ppu/runtime.h"

namespace pspocket::hle::test {

inline constexpr u32 kData = 0x00100000;   // scratch guest data (2 MB, committed)
inline constexpr u32 kStacks = 0x00300000; // one 64 KB guest stack per test thread (16 of them)

inline u64 r3_of(lv2::CellError e) { return lv2::to_r3(lv2::err(e)); }
inline u64 r3_of_code(u32 code) { return static_cast<u64>(static_cast<s64>(static_cast<s32>(code))); }

struct Fixture {
    GuestMemory mem;
    std::unique_ptr<lv2::Kernel> kernel;
    std::unique_ptr<HleProcess> hle;
    // Catch2's assertions are not thread-safe (D-571): checks that fail on a test's own std::threads are counted here,
    // and the count is checked on the test's thread when the fixture goes away.
    std::thread::id test_thread = std::this_thread::get_id();
    std::atomic<u32> off_thread_failures{0};

    explicit Fixture(ProcessConfig config = {}, lv2::Config kconfig = {})
    {
        REQUIRE(mem.reserve());
        kernel = std::make_unique<lv2::Kernel>(mem, std::move(kconfig));
        kernel->install();
        hle = std::make_unique<HleProcess>(*kernel, 0x10000, 0x10000000, std::move(config));
        hle->install();
        REQUIRE(hle->main_memory().reserve(kData, 0x300000));
        REQUIRE(mem.commit(kData, 0x300000));
    }
    ~Fixture()
    {
        kernel->shutdown();
        hle.reset();
        kernel.reset();
        if (std::uncaught_exceptions() != 0) return;
        try { // a destructor must not throw: Catch2 throws even from CHECK once --abort is stopping the run
            CHECK(off_thread_failures == 0);
        } catch (...) {
        }
    }
    u8* base() { return mem.base(); }
    bool on_test_thread() const { return std::this_thread::get_id() == test_thread; }
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

    static const Function* lookup(std::string_view name)
    {
        const Function* fn = nullptr;
        for (const Function& x : registry().functions())
            if (x.name == name) fn = &x;
        return fn;
    }

    static const Function& find(std::string_view name)
    {
        const Function* fn = lookup(name);
        REQUIRE(fn != nullptr);
        return *fn;
    }

    // Callable from a test's own std::threads too: there a failed check is counted in the fixture (see Fixture).
    template <class... A>
    u64 call(std::string_view name, A... args)
    {
        const bool here = f.on_test_thread();
        const Function* fn = lookup(name);
        if (here) REQUIRE(fn != nullptr);
        if (!fn) {
            f.off_thread_failures++;
            return ~u64{0};
        }
        const u64 v[] = {0, static_cast<u64>(args)...}; // leading 0: no zero-length array for calls without arguments
        for (usize i = 0; i < sizeof...(A); i++) {
            if (i < 8) ctx.gpr[3 + i] = v[i + 1];
            else ps_write64(f.base(), static_cast<u32>(ctx.gpr[1]) + 0x70 + 8 * static_cast<u32>(i - 8), v[i + 1]);
        }
        const u64 sp = ctx.gpr[1];
        fn->fn(ctx, f.base());
        if (here) CHECK(ctx.gpr[1] == sp); // stack variables released
        else if (ctx.gpr[1] != sp) f.off_thread_failures++;
        return ctx.gpr[3];
    }
};

} // namespace pspocket::hle::test
