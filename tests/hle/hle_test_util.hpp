#pragma once

// Shared fixture for HLE tests: a reserved guest address space, an lv2 kernel and an HLE process, and host threads
// with an lv2 identity that call HLE functions the way guest code does (arguments in r3..r10, then the stack).
// Everything is synthetic (no game data). test_sysPrxForUser.cpp has its own copy of the same pattern.
//
// Threads (D-571, D-585): code that runs on a test's own std::thread or a kernel thread checks with THREAD_CHECK and
// THREAD_REQUIRE (tests/common/thread_check.hpp), never with Catch2's CHECK and REQUIRE, which are not thread-safe. A
// guest word that another thread writes while the test watches it, or reads while the test writes it, is read and
// written as a PPU thread orders it (load32_acquire, store32_release, line_copy below), so ThreadSanitizer sees the
// order the guest's barriers give and the test reads nothing before it is written.

#include <catch_amalgamated.hpp>

#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <string_view>
#include <thread>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/reservation.h"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/ppu/runtime.h"

#include "thread_check.hpp"

namespace pspocket::hle::test {

inline constexpr u32 kData = 0x00100000;   // scratch guest data (2 MB, committed)
inline constexpr u32 kStacks = 0x00300000; // one 64 KB guest stack per test thread (16 of them)

inline u64 r3_of(lv2::CellError e) { return lv2::to_r3(lv2::err(e)); }
inline u64 r3_of_code(u32 code) { return static_cast<u64>(static_cast<s64>(static_cast<s32>(code))); }

// The test standing in for a PPU thread (D-585): its load before an isync or lwsync is an acquire load (a flag, a count
// or a done word another thread writes: what that thread wrote before it is then visible), its store after an lwsync a
// release store (a flag or put another thread polls). Aligned guest words only.
inline u32 load32_acquire(u8* base, u32 addr)
{
    return ps_bswap32(__atomic_load_n(reinterpret_cast<u32*>(base + addr), __ATOMIC_ACQUIRE));
}
inline u64 load64_acquire(u8* base, u32 addr)
{
    return ps_bswap64(__atomic_load_n(reinterpret_cast<u64*>(base + addr), __ATOMIC_ACQUIRE));
}
inline void store8_release(u8* base, u32 addr, u8 v) { __atomic_store_n(base + addr, v, __ATOMIC_RELEASE); }
inline void store32_release(u8* base, u32 addr, u32 v)
{
    __atomic_store_n(reinterpret_cast<u32*>(base + addr), ps_bswap32(v), __ATOMIC_RELEASE);
}
inline void store64_release(u8* base, u32 addr, u64 v)
{
    __atomic_store_n(reinterpret_cast<u64*>(base + addr), ps_bswap64(v), __ATOMIC_RELEASE);
}

// A consistent copy of a guest line through the reservation system, as lwarx and GETLLAR read it. Where another thread
// changes the line with reservation_op, PUTLLC or stwcx. (the SPURS kernel's states and bits, a queue's or a job
// chain's words), a copy that shows its change also shows what that thread wrote before it: the line's version orders
// them.
struct LineCopy {
    alignas(16) u8 bytes[PS_RES_LINE_SIZE];
    u8 byte(u32 off) const { return bytes[off]; }
    u16 half(u32 off) const { return ps_ld16(bytes + off); }
    u32 word(u32 off) const { return ps_ld32(bytes + off); }
    u64 dword(u32 off) const { return ps_ld64(bytes + off); }
};
inline LineCopy line_copy(u8* base, u32 addr)
{
    LineCopy l;
    ps_res_acquire(base, addr, l.bytes);
    return l;
}
// One field read through line_copy: `addr` need not be the line's start.
inline u8 line_byte(u8* base, u32 addr) { return line_copy(base, addr).byte(addr % PS_RES_LINE_SIZE); }
inline u16 line_half(u8* base, u32 addr) { return line_copy(base, addr).half(addr % PS_RES_LINE_SIZE); }
inline u32 line_word(u8* base, u32 addr) { return line_copy(base, addr).word(addr % PS_RES_LINE_SIZE); }
inline u64 line_dword(u8* base, u32 addr) { return line_copy(base, addr).dword(addr % PS_RES_LINE_SIZE); }

struct Fixture {
    GuestMemory mem;
    std::unique_ptr<lv2::Kernel> kernel;
    std::unique_ptr<HleProcess> hle;

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
        pspocket::test::report_thread_checks(); // what failed on the test's own threads and the kernel's
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

    static const Function* lookup(std::string_view name)
    {
        const Function* fn = nullptr;
        for (const Function& x : registry().functions())
            if (x.name == name) fn = &x;
        return fn;
    }

    // Callable from a test's own std::threads too (THREAD_REQUIRE, D-585).
    static const Function& find(std::string_view name)
    {
        const Function* fn = lookup(name);
        if (!fn) std::fprintf(stderr, "no HLE function named %.*s\n", static_cast<int>(name.size()), name.data());
        THREAD_REQUIRE(fn != nullptr);
        return *fn;
    }

    // Callable from a test's own std::threads too (THREAD_CHECK and THREAD_REQUIRE, D-585).
    template <class... A>
    u64 call(std::string_view name, A... args)
    {
        const Function& fn = find(name);
        const u64 v[] = {0, static_cast<u64>(args)...}; // leading 0: no zero-length array for calls without arguments
        for (usize i = 0; i < sizeof...(A); i++) {
            if (i < 8) ctx.gpr[3 + i] = v[i + 1];
            else ps_write64(f.base(), static_cast<u32>(ctx.gpr[1]) + 0x70 + 8 * static_cast<u32>(i - 8), v[i + 1]);
        }
        const u64 sp = ctx.gpr[1];
        fn.fn(ctx, f.base());
        THREAD_CHECK(ctx.gpr[1] == sp); // stack variables released
        return ctx.gpr[3];
    }
};

} // namespace pspocket::hle::test
