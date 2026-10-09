// cellSync2 (D-531) against libsync2, Sony's own module of the library, from the user's installed system software
// (firmware_compare.hpp; skipped without it): the same scripts of calls on objects at the same addresses go to both,
// and the answers and every byte of the objects and their buffers must be the same; objects one made, the other works
// on; and threads blocking on each one's mutex, condition, semaphore and queue. No game data.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "firmware_compare.hpp"
#include "hle_test_util.hpp"
#include "pspocket/core/be.h"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kAttr = kData + 0x100, kOut = kData + 0x200;
constexpr u32 kMutex = kData + 0x1000, kMutexBuf = kData + 0x1100, kMutex2 = kData + 0x1380;
constexpr u32 kCond = kData + 0x1400, kCondBuf = kData + 0x1500;
constexpr u32 kSem = kData + 0x1800, kSemBuf = kData + 0x1900, kSem2 = kData + 0x1C00;
constexpr u32 kQueue = kData + 0x2000, kQueueBuf = kData + 0x3000;
constexpr u32 kElems = kData + 0x8000, kPtrs = kData + 0x9000, kPopped = kData + 0xA000;
constexpr u32 kArea = kData + 0x1000, kAreaEnd = kData + 0xB000;

// One side's call of a cellSync2 function by name: r3's low word.
using Call = std::function<u32(const char*, std::initializer_list<u64>)>;

Call mine(GuestThread& t)
{
    return [&t](const char* name, std::initializer_list<u64> args) {
        const Function& fn = GuestThread::find(name);
        u32 r = 3;
        for (const u64 a : args) t.ctx.gpr[r++] = a;
        fn.fn(t.ctx, t.f.base());
        return static_cast<u32>(t.ctx.gpr[3]);
    };
}

Call sonys(GuestThread& t, const FirmwareLibrary& sony)
{
    return [&t, &sony](const char* name, std::initializer_list<u64> args) {
        const u32 opd = sony.module->functions.at({"cellSync2", recomp::nid_of(name)});
        return static_cast<u32>(call_guest(t.ctx, opd, args));
    };
}

void attr(u8* b, std::initializer_list<std::pair<u32, u32>> u32s, std::initializer_list<std::pair<u32, u16>> u16s = {})
{
    for (const auto& [at, v] : u32s) ps_write32(b, kAttr + at, v);
    for (const auto& [at, v] : u16s) ps_write16(b, kAttr + at, v);
}

// A made-up script over every function on one thread: attributes, sizes, errors in each argument, the objects' states
// after each step. The log is answers and the bytes the steps leave.
std::vector<std::string> script(u8* b, const Call& call)
{
    std::vector<std::string> log;
    std::memset(b + kData, 0, 0xB000);
    const auto step = [&](const char* name, std::initializer_list<u64> args) {
        const u32 r = call(name, args);
        log.push_back(std::format("{} -> {:#x}", name, r));
        return r;
    };
    const auto snapshot = [&](std::string_view what) {
        std::string s = std::format("{}:", what);
        for (u32 a = kArea; a < kAreaEnd; a += 4)
            if (const u32 w = ps_read32(b, a)) s += std::format(" {:x}={:x}", a - kArea, w);
        s += " attr";
        for (u32 a = kAttr; a < kAttr + 0x80; a += 4) s += std::format(" {:x}", ps_read32(b, a));
        s += std::format(" out {:x} {:x}", ps_read32(b, kOut), ps_read32(b, kOut + 4));
        log.push_back(s);
    };

    // Attributes.
    step("_cellSync2MutexAttributeInitialize", {0, 0x330000});
    step("_cellSync2MutexAttributeInitialize", {kAttr, 0x330000});
    snapshot("mutex attr");
    step("_cellSync2CondAttributeInitialize", {kAttr, 0x123});
    snapshot("cond attr");
    step("_cellSync2SemaphoreAttributeInitialize", {kAttr, 0x280000});
    snapshot("sem attr");
    step("_cellSync2QueueAttributeInitialize", {kAttr, 0x2F0000});
    snapshot("queue attr");

    // Sizes: every range edge.
    for (const u32 sdk : {0u, 0x26FFFFu, 0x270000u, 0x330000u, 0x330001u}) {
        for (const u16 waiters : {u16{0}, u16{1}, u16{2}, u16{15}, u16{0x8000}, u16{0x8001}}) {
            step("_cellSync2MutexAttributeInitialize", {kAttr, sdk});
            attr(b, {}, {{6, waiters}});
            step("cellSync2MutexEstimateBufferSize", {kAttr, kOut});
            step("_cellSync2CondAttributeInitialize", {kAttr, sdk});
            attr(b, {}, {{4, waiters}});
            step("cellSync2CondEstimateBufferSize", {kAttr, kOut});
            step("_cellSync2SemaphoreAttributeInitialize", {kAttr, sdk});
            attr(b, {}, {{6, waiters}});
            step("cellSync2SemaphoreEstimateBufferSize", {kAttr, kOut});
            snapshot(std::format("sizes sdk {:x} waiters {:x}", sdk, waiters));
        }
    }
    step("cellSync2MutexEstimateBufferSize", {0, kOut});
    step("_cellSync2MutexAttributeInitialize", {kAttr, 0x330000});
    step("cellSync2MutexEstimateBufferSize", {kAttr, 0});
    for (const u32 types : {0u, 1u, 0x10Fu, 0x10u, 0x20u, 0x200u, 0x4000u, 0x8001u}) {
        for (const u32 element : {0u, 0x10u, 0x18u, 0x4000u, 0x4010u}) {
            for (const u32 depth : {0u, 1u, 3u, 63u, 64u, 0xFFFFFFFCu, 0xFFFFFFFDu}) {
                step("_cellSync2QueueAttributeInitialize", {kAttr, 0x330000});
                attr(b, {{4, types}, {8, element}, {0xC, depth}});
                step("cellSync2QueueEstimateBufferSize", {kAttr, kOut});
                log.back() += std::format(" size {:x}", ps_read32(b, kOut));
            }
        }
    }
    step("_cellSync2QueueAttributeInitialize", {kAttr, 0x330000});
    attr(b, {}, {{0x10, 0x8001}});
    step("cellSync2QueueEstimateBufferSize", {kAttr, kOut});
    attr(b, {}, {{0x10, 0}, {0x12, 0}});
    step("cellSync2QueueEstimateBufferSize", {kAttr, kOut});
    snapshot("queue sizes");

    // Mutex.
    step("_cellSync2MutexAttributeInitialize", {kAttr, 0x330000});
    step("cellSync2MutexInitialize", {0, kMutexBuf, kAttr});
    step("cellSync2MutexInitialize", {kMutex + 8, kMutexBuf, kAttr});
    step("cellSync2MutexInitialize", {kMutex, 0, kAttr});
    step("cellSync2MutexInitialize", {kMutex, kMutexBuf + 4, kAttr});
    attr(b, {}, {{4, 0x30}});
    step("cellSync2MutexInitialize", {kMutex, kMutexBuf, kAttr});
    attr(b, {}, {{4, 0}});
    step("cellSync2MutexInitialize", {kMutex, kMutexBuf, kAttr});
    attr(b, {}, {{4, 0x10F}});
    step("cellSync2MutexInitialize", {kMutex, kMutexBuf, kAttr});
    snapshot("mutex");
    step("cellSync2MutexTryLock", {kMutex, 0});
    snapshot("mutex locked");
    step("cellSync2MutexTryLock", {kMutex, 0}); // DEADLK: its own, not recursive
    step("cellSync2MutexLock", {kMutex, 0});
    step("cellSync2MutexFinalize", {kMutex});
    step("cellSync2MutexUnlock", {kMutex, 0});
    step("cellSync2MutexUnlock", {kMutex, 0}); // STAT
    step("cellSync2MutexLock", {kMutex, 0});
    step("cellSync2MutexUnlock", {kMutex, 0});
    snapshot("mutex unlocked");
    // Recursive, no waiters.
    step("_cellSync2MutexAttributeInitialize", {kAttr, 0x330000});
    attr(b, {}, {{6, 0}});
    b[kAttr + 8] = 1;
    step("cellSync2MutexInitialize", {kMutex2, 0, kAttr});
    step("cellSync2MutexLock", {kMutex2, 0}); // NOMEM: no waiters
    step("cellSync2MutexTryLock", {kMutex2, 0});
    step("cellSync2MutexTryLock", {kMutex2, 0});
    step("cellSync2MutexTryLock", {kMutex2, 0});
    snapshot("recursive x3");
    ps_write32(b, kMutex2 + 0x14, 0xFFFFFFFF);
    step("cellSync2MutexTryLock", {kMutex2, 0}); // AGAIN
    ps_write32(b, kMutex2 + 0x14, 3);
    step("cellSync2MutexUnlock", {kMutex2, 0});
    step("cellSync2MutexUnlock", {kMutex2, 0});
    snapshot("recursive x1");
    step("cellSync2MutexUnlock", {kMutex2, 0});
    step("cellSync2MutexFinalize", {kMutex2});
    // A configuration with a NULL caller.
    ps_write32(b, kOut + 0x20, 0);
    step("cellSync2MutexTryLock", {kMutex, kOut + 0x20});
    step("cellSync2MutexUnlock", {kMutex, kOut + 0x20});

    // Condition.
    step("_cellSync2CondAttributeInitialize", {kAttr, 0x330000});
    step("cellSync2CondInitialize", {kCond, kMutex, kCondBuf, kAttr});
    snapshot("cond");
    step("cellSync2MutexFinalize", {kMutex}); // BUSY: a condition on it
    step("cellSync2CondSignal", {kCond, 0});
    step("cellSync2CondSignalAll", {kCond, 0});
    step("cellSync2CondWait", {kCond, 0}); // STAT: mutex not locked
    step("cellSync2CondFinalize", {kCond});
    snapshot("cond finalized");
    attr(b, {}, {{4, 16}});
    step("cellSync2CondInitialize", {kCond, kMutex, kCondBuf, kAttr}); // more waiters than the mutex has
    step("cellSync2CondInitialize", {kCond, kMutex, kCondBuf + 4, kAttr});
    step("cellSync2CondInitialize", {kCond + 4, kMutex, kCondBuf, kAttr});

    // Semaphore.
    step("_cellSync2SemaphoreAttributeInitialize", {kAttr, 0x330000});
    step("cellSync2SemaphoreInitialize", {kSem, kSemBuf, 5, kAttr}); // INVAL: one waiter and a buffer
    for (const s32 v : {-0x800001, -0x800000, 0, 0x7FFFFF, 0x800000})
        step("cellSync2SemaphoreInitialize", {kSem, 0, static_cast<u32>(v), kAttr});
    step("cellSync2SemaphoreInitialize", {kSem, 0, 5, kAttr});
    snapshot("sem 5");
    step("cellSync2SemaphoreGetCount", {kSem, kOut});
    step("cellSync2SemaphoreTryAcquire", {kSem, 3, 0});
    step("cellSync2SemaphoreTryAcquire", {kSem, 3, 0}); // AGAIN
    step("cellSync2SemaphoreAcquire", {kSem, 2, 0});
    step("cellSync2SemaphoreTryAcquire", {kSem, 0x800000, 0});
    step("cellSync2SemaphoreRelease", {kSem, 0x7FFFFF, 0});
    step("cellSync2SemaphoreRelease", {kSem, 1, 0}); // STAT: past 0x7FFFFF
    step("cellSync2SemaphoreRelease", {kSem, 0x800000, 0});
    step("cellSync2SemaphoreGetCount", {kSem, kOut});
    snapshot("sem full");
    step("cellSync2SemaphoreFinalize", {kSem});
    step("_cellSync2SemaphoreAttributeInitialize", {kAttr, 0x330000});
    attr(b, {}, {{6, 4}});
    step("cellSync2SemaphoreInitialize", {kSem2, 0, 0, kAttr}); // NULL: four waiters, no buffer
    step("cellSync2SemaphoreInitialize", {kSem2, kSemBuf, static_cast<u32>(-3), kAttr});
    step("cellSync2SemaphoreAcquire", {kSem2, 2, 0}); // INVAL: more than one at a time with several waiters
    step("cellSync2SemaphoreRelease", {kSem2, 5, 0});
    step("cellSync2SemaphoreTryAcquire", {kSem2, 2, 0});
    snapshot("sem2");

    // Queue: 16-byte elements, depth 5, three of each kind of waiter.
    step("_cellSync2QueueAttributeInitialize", {kAttr, 0x330000});
    attr(b, {{8, 0x10}, {0xC, 5}}, {{0x10, 3}, {0x12, 3}});
    step("cellSync2QueueEstimateBufferSize", {kAttr, kOut});
    step("cellSync2QueueInitialize", {kQueue, kQueueBuf + 0x40, kAttr});
    step("cellSync2QueueInitialize", {kQueue, kQueueBuf, kAttr});
    snapshot("queue");
    for (u32 i = 0; i < 64; i++) ps_write32(b, kElems + 4 * i, 0xE1E10000 + i);
    for (u32 i = 0; i < 8; i++) ps_write32(b, kPtrs + 4 * i, kElems + 0x10 * i);
    step("cellSync2QueueGetDepth", {kQueue, kOut});
    step("cellSync2QueueGetSize", {kQueue, kOut + 4});
    step("cellSync2QueueTryPop", {kQueue, kPopped, 0}); // AGAIN
    step("cellSync2QueuePush", {kQueue, kElems, 0});
    step("cellSync2QueueTryPush", {kQueue, kPtrs + 4, 3, 0});
    step("cellSync2QueueTryPush", {kQueue, kPtrs, 2, 0}); // AGAIN: one entry left
    step("cellSync2QueueTryPush", {kQueue, kPtrs + 16, 1, 0});
    step("cellSync2QueueGetSize", {kQueue, kOut + 4});
    snapshot("queue full");
    for (u32 i = 0; i < 3; i++) step("cellSync2QueuePop", {kQueue, kPopped + 0x10 * i, 0});
    step("cellSync2QueueTryPush", {kQueue, kPtrs + 8, 3, 0});
    for (u32 i = 3; i < 9; i++) step("cellSync2QueueTryPop", {kQueue, kPopped + 0x10 * i, 0});
    step("cellSync2QueueGetSize", {kQueue, kOut + 4});
    snapshot("queue after");
    step("cellSync2QueueTryPush", {kQueue, kPtrs, 0, 0}); // INVAL
    step("cellSync2QueueFinalize", {kQueue});
    // No pushers' waiters: Push is NOMEM, TryPush works.
    attr(b, {}, {{0x10, 0}});
    step("cellSync2QueueInitialize", {kQueue, kQueueBuf, kAttr});
    step("cellSync2QueuePush", {kQueue, kElems, 0});
    step("cellSync2QueueTryPush", {kQueue, kPtrs, 4, 0});
    snapshot("queue no pushers");
    return log;
}

void compare_logs(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    usize differ = 0;
    for (usize i = 0; i < std::max(a.size(), b.size()); i++) {
        const std::string x = i < a.size() ? a[i] : "(none)", y = i < b.size() ? b[i] : "(none)";
        if (x == y) continue;
        if (++differ <= 40) UNSCOPED_INFO(std::format("{}: {}\n  (pspocket)\n  {}\n  (Sony)", i, x, y));
    }
    INFO(std::format("{} of {} lines differ", differ, b.size()));
    CHECK(differ == 0);
}

// Threads on one side, each its own guest thread.
struct Side {
    Fixture& f;
    const FirmwareLibrary* sony; // null: pspocket's
    u64 call(GuestThread& g, const char* name, std::initializer_list<u64> args) const
    {
        if (sony) return call_guest(g.ctx, sony->module->functions.at({"cellSync2", recomp::nid_of(name)}), args);
        const Function& fn = GuestThread::find(name);
        u32 r = 3;
        for (const u64 a : args) g.ctx.gpr[r++] = a;
        fn.fn(g.ctx, f.base());
        return g.ctx.gpr[3];
    }
};

// Blocking: four threads on a mutex with eight waiters; a condition; a semaphore with four waiters; a queue between
// two producers and two consumers. Run on one side, checked for what they must do.
void blocking(Fixture& f, GuestThread& t, const Side& side)
{
    u8* b = f.base();
    std::memset(b + kData, 0, 0xB000);
    const auto call = [&](const char* name, std::initializer_list<u64> args) { return static_cast<u32>(side.call(t, name, args)); };
    // Mutex.
    REQUIRE(call("_cellSync2MutexAttributeInitialize", {kAttr, 0x330000}) == 0);
    attr(b, {}, {{6, 8}});
    REQUIRE(call("cellSync2MutexInitialize", {kMutex, kMutexBuf, kAttr}) == 0);
    std::atomic<u32> failures{0};
    u32 counter = 0;
    {
        std::vector<std::thread> threads;
        for (u32 i = 0; i < 4; i++) {
            threads.emplace_back([&, i] {
                GuestThread g(f, 1 + i, "locker");
                for (u32 k = 0; k < 150; k++) {
                    if (side.call(g, "cellSync2MutexLock", {kMutex, 0}) != 0) failures++;
                    const u32 c = counter;
                    if (k % 16 == 0) std::this_thread::yield();
                    counter = c + 1;
                    if (side.call(g, "cellSync2MutexUnlock", {kMutex, 0}) != 0) failures++;
                }
            });
        }
        for (auto& th : threads) th.join();
    }
    CHECK(failures == 0);
    CHECK(counter == 600);
    CHECK(call("cellSync2MutexFinalize", {kMutex}) == 0);

    // Condition: waiters wake on a flag set under the mutex.
    REQUIRE(call("cellSync2MutexInitialize", {kMutex, kMutexBuf, kAttr}) == 0);
    REQUIRE(call("_cellSync2CondAttributeInitialize", {kAttr, 0x330000}) == 0);
    attr(b, {}, {{4, 4}});
    REQUIRE(call("cellSync2CondInitialize", {kCond, kMutex, kCondBuf, kAttr}) == 0);
    std::atomic<u32> woke{0};
    std::atomic<u32> flag{0};
    {
        std::vector<std::thread> threads;
        for (u32 i = 0; i < 3; i++) {
            threads.emplace_back([&, i] {
                GuestThread g(f, 5 + i, "waiter");
                if (side.call(g, "cellSync2MutexLock", {kMutex, 0}) != 0) failures++;
                while (!flag)
                    if (side.call(g, "cellSync2CondWait", {kCond, 0}) != 0) failures++;
                woke++;
                if (side.call(g, "cellSync2MutexUnlock", {kMutex, 0}) != 0) failures++;
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(call("cellSync2MutexLock", {kMutex, 0}) == 0);
        flag = 1;
        CHECK(call("cellSync2CondSignalAll", {kCond, 0}) == 0);
        CHECK(call("cellSync2MutexUnlock", {kMutex, 0}) == 0);
        for (auto& th : threads) th.join();
    }
    CHECK(woke == 3);
    CHECK(call("cellSync2CondFinalize", {kCond}) == 0);
    CHECK(call("cellSync2MutexFinalize", {kMutex}) == 0);

    // Semaphore: three acquirers wait for releases.
    REQUIRE(call("_cellSync2SemaphoreAttributeInitialize", {kAttr, 0x330000}) == 0);
    attr(b, {}, {{6, 4}});
    REQUIRE(call("cellSync2SemaphoreInitialize", {kSem, kSemBuf, 0, kAttr}) == 0);
    {
        std::vector<std::thread> threads;
        std::atomic<u32> got{0};
        for (u32 i = 0; i < 3; i++) {
            threads.emplace_back([&, i] {
                GuestThread g(f, 8 + i, "acquirer");
                if (side.call(g, "cellSync2SemaphoreAcquire", {kSem, 1, 0}) != 0) failures++;
                got++;
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(got == 0);
        CHECK(call("cellSync2SemaphoreRelease", {kSem, 2, 0}) == 0);
        CHECK(call("cellSync2SemaphoreRelease", {kSem, 2, 0}) == 0);
        for (auto& th : threads) th.join();
        CHECK(got == 3);
    }
    REQUIRE(call("cellSync2SemaphoreGetCount", {kSem, kOut}) == 0);
    CHECK(ps_read32(b, kOut) == 1);
    CHECK(call("cellSync2SemaphoreFinalize", {kSem}) == 0);

    // Queue: depth 4 between two producers and two consumers.
    REQUIRE(call("_cellSync2QueueAttributeInitialize", {kAttr, 0x330000}) == 0);
    attr(b, {{8, 0x10}, {0xC, 4}}, {{0x10, 4}, {0x12, 4}});
    REQUIRE(call("cellSync2QueueInitialize", {kQueue, kQueueBuf, kAttr}) == 0);
    {
        std::vector<std::thread> threads;
        std::atomic<u64> sum{0};
        for (u32 i = 0; i < 2; i++) {
            threads.emplace_back([&, i] {
                GuestThread g(f, 11 + i, "producer");
                const u32 elem = kElems + 0x100 * i;
                for (u32 k = 1; k <= 100; k++) {
                    ps_write32(b, elem, i * 1000 + k);
                    if (side.call(g, "cellSync2QueuePush", {kQueue, elem, 0}) != 0) failures++;
                }
            });
            threads.emplace_back([&, i] {
                GuestThread g(f, 13 + i, "consumer");
                const u32 out = kPopped + 0x100 * i;
                for (u32 k = 0; k < 100; k++) {
                    if (side.call(g, "cellSync2QueuePop", {kQueue, out, 0}) != 0) failures++;
                    sum += ps_read32(b, out);
                }
            });
        }
        for (auto& th : threads) th.join();
        CHECK(sum == 2 * 5050 + 1000 * 100);
    }
    REQUIRE(call("cellSync2QueueGetSize", {kQueue, kOut}) == 0);
    CHECK(ps_read32(b, kOut) == 0);
    CHECK(call("cellSync2QueueFinalize", {kQueue}) == 0);
    CHECK(failures == 0);
}

} // namespace

TEST_CASE("cellSync2: one thread's script, and threads blocking on each object (pspocket)", "[hle][sync2]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    const std::vector<std::string> log = script(f.base(), mine(t));
    CHECK(log.size() > 300);
    CHECK(std::find(log.begin(), log.end(), "cellSync2MutexTryLock -> 0x0") != log.end());
    blocking(f, t, Side{f, nullptr});
}

TEST_CASE("cellSync2 against libsync2: answers and bytes, objects across implementations, blocking", "[hle][lle][sync2]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    Fixture f;
    GuestThread t(f, 0, "ppu");
    t.call("sys_initialize_tls", 0, 0, 0, 0);
    FirmwareLibrary sony(f, t, *flash, "cellSync2", true);
    u8* b = f.base();
    const std::vector<std::string> ours = script(b, mine(t));
    const std::vector<std::string> theirs = script(b, sonys(t, sony));
    compare_logs(ours, theirs);

    // One side makes the objects and the other works on them (one thread: no receivers cross over).
    const auto across = [&](const Call& maker, const Call& user) {
        std::vector<std::string> log;
        std::memset(b + kData, 0, 0xB000);
        const auto both = [&](const Call& c, const char* name, std::initializer_list<u64> args) {
            log.push_back(std::format("{} -> {:#x}", name, c(name, args)));
        };
        both(maker, "_cellSync2MutexAttributeInitialize", {kAttr, 0x330000});
        b[kAttr + 8] = 1;
        both(maker, "cellSync2MutexInitialize", {kMutex, kMutexBuf, kAttr});
        both(user, "cellSync2MutexLock", {kMutex, 0});
        both(maker, "cellSync2MutexTryLock", {kMutex, 0});
        both(user, "cellSync2MutexUnlock", {kMutex, 0});
        both(maker, "cellSync2MutexUnlock", {kMutex, 0});
        both(user, "cellSync2MutexFinalize", {kMutex});
        both(maker, "_cellSync2QueueAttributeInitialize", {kAttr, 0x330000});
        attr(b, {{8, 0x20}, {0xC, 6}}, {{0x10, 2}, {0x12, 2}});
        both(maker, "cellSync2QueueInitialize", {kQueue, kQueueBuf, kAttr});
        for (u32 i = 0; i < 64; i++) ps_write32(b, kElems + 4 * i, 0xC0DE0000 + i);
        for (u32 i = 0; i < 8; i++) ps_write32(b, kPtrs + 4 * i, kElems + 0x20 * i);
        both(user, "cellSync2QueuePush", {kQueue, kElems, 0});
        both(maker, "cellSync2QueueTryPush", {kQueue, kPtrs + 4, 4, 0});
        both(user, "cellSync2QueuePop", {kQueue, kPopped, 0});
        both(maker, "cellSync2QueueTryPop", {kQueue, kPopped + 0x20, 0});
        both(user, "cellSync2QueueTryPush", {kQueue, kPtrs, 3, 0});
        both(maker, "_cellSync2SemaphoreAttributeInitialize", {kAttr, 0x330000});
        both(maker, "cellSync2SemaphoreInitialize", {kSem, 0, 7, kAttr});
        both(user, "cellSync2SemaphoreAcquire", {kSem, 4, 0});
        both(maker, "cellSync2SemaphoreRelease", {kSem, 2, 0});
        both(user, "cellSync2SemaphoreGetCount", {kSem, kOut});
        std::string bytes;
        for (u32 a = kArea; a < kAreaEnd; a += 4)
            if (const u32 w = ps_read32(b, a)) bytes += std::format(" {:x}={:x}", a - kArea, w);
        log.push_back(bytes);
        return log;
    };
    const Call m = mine(t), s = sonys(t, sony);
    const auto all_sony = across(s, s);
    compare_logs(across(m, s), all_sony);
    compare_logs(across(s, m), all_sony);
    compare_logs(across(m, m), all_sony);

    blocking(f, t, Side{f, nullptr});
    blocking(f, t, Side{f, &sony});
}
