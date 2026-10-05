// cellSync mutexes as RPCS3 has them (runtime/hle/src/cellSync.cpp), synthetic: no game data. The three functions
// Assassin's Creed imports; RPCS3's checks (NULL_POINTER, then ALIGN; FAULT for a mutex that is not guest memory,
// D-042); Initialize freeing the mutex whatever it held; TryLock taking a free mutex once and reporting BUSY until
// Unlock, with both 16-bit counters wrapping; and the updates going through the reservation system: a TryLock that
// finds the mutex held leaves a guest reservation on the line valid and wakes nothing, every write fails such a
// reservation and wakes what waits for the line. Then host threads hammer TryLock and Unlock on one mutex together with
// guest-style users of the same word, a PPU thread with lwarx/stwcx. and an SPU taking tickets with GETLLAR/PUTLLC, and
// the mutex never has two holders.

#include "hle_test_util.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pspocket/core/reservation.h"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u64 kBusy = 0xFFFFFFFF8041010Aull, kFault = 0xFFFFFFFF8041010Dull, kAlign = 0xFFFFFFFF80410110ull,
              kNullPointer = 0xFFFFFFFF80410111ull;

constexpr u32 kLine = kData + 0x1000; // a 128-byte line: the mutex and data around it
constexpr u32 kMutex = kLine + 0x44;  // CellSyncMutex {be16 rel, be16 acq}, the second half of an 8-byte word
constexpr u32 kRel = 0, kAcq = 2;     // its fields
constexpr u32 kUnmapped = 0x0FFF0000; // in the main area, never committed by these tests

// The mutex as one big-endian word: rel in the high half, acq in the low half.
u32 word(u8* b) { return ps_read32(b, kMutex); }

std::array<u8, PS_RES_LINE_SIZE> line_bytes(u8* b)
{
    std::array<u8, PS_RES_LINE_SIZE> out;
    std::memcpy(out.data(), b + kLine, out.size());
    return out;
}

} // namespace

TEST_CASE("cellSync: the three mutex functions Assassin's Creed imports", "[hle][sync]")
{
    const std::pair<std::string_view, u32> expected[] = {
        {"cellSyncMutexInitialize", 0xA9072DEE},
        {"cellSyncMutexTryLock", 0xD06918C4},
        {"cellSyncMutexUnlock", 0x91F2B7B0},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const auto& [name, nid] : expected) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("cellSync", nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
    }
}

TEST_CASE("cellSyncMutexInitialize: RPCS3's checks, then a free mutex; the rest of the line is untouched", "[hle][sync]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    for (u32 i = 0; i < PS_RES_LINE_SIZE; i++) ps_write8(b, kLine + i, static_cast<u8>(0x80 + i));
    auto expected = line_bytes(b);

    CHECK(a.call("cellSyncMutexInitialize", 0) == kNullPointer);
    for (const u32 misaligned : {kMutex + 1, kMutex + 2, kMutex + 3, kUnmapped + 2}) { // alignment before memory
        INFO(misaligned);
        CHECK(a.call("cellSyncMutexInitialize", misaligned) == kAlign);
    }
    CHECK(a.call("cellSyncMutexInitialize", kUnmapped) == kFault);
    CHECK(line_bytes(b) == expected); // refused calls write nothing

    REQUIRE(a.call("cellSyncMutexInitialize", kMutex) == 0);
    CHECK(word(b) == 0); // rel 0, acq 0: free
    std::memset(expected.data() + (kMutex - kLine), 0, 4);
    CHECK(line_bytes(b) == expected); // only the mutex's 4 bytes changed

    // Whatever the mutex held (here rel 5, acq 7: a holder and a waiter), it is free afterwards.
    ps_write32(b, kMutex, 0x00050007);
    REQUIRE(a.call("cellSyncMutexInitialize", kMutex) == 0);
    CHECK(word(b) == 0);
    CHECK(a.call("cellSyncMutexTryLock", kMutex) == 0);
}

TEST_CASE("cellSyncMutexTryLock and cellSyncMutexUnlock: RPCS3's checks and its ticket lock", "[hle][sync]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    REQUIRE(a.call("cellSyncMutexInitialize", kMutex) == 0);

    for (const char* fn : {"cellSyncMutexTryLock", "cellSyncMutexUnlock"}) {
        INFO(fn);
        CHECK(a.call(fn, 0) == kNullPointer);
        CHECK(a.call(fn, kMutex + 2) == kAlign);
        CHECK(a.call(fn, kUnmapped + 1) == kAlign);
        CHECK(a.call(fn, kUnmapped) == kFault);
        CHECK(word(b) == 0);
    }

    // TryLock takes a free mutex (acq + 1); while it is held, TryLock returns BUSY and changes nothing.
    REQUIRE(a.call("cellSyncMutexTryLock", kMutex) == 0);
    CHECK(word(b) == 0x00000001);
    CHECK(a.call("cellSyncMutexTryLock", kMutex) == kBusy);
    CHECK(a.call("cellSyncMutexTryLock", kMutex) == kBusy);
    CHECK(word(b) == 0x00000001);
    // Unlock releases it (rel + 1), and TryLock takes it again.
    REQUIRE(a.call("cellSyncMutexUnlock", kMutex) == 0);
    CHECK(word(b) == 0x00010001);
    REQUIRE(a.call("cellSyncMutexTryLock", kMutex) == 0);
    CHECK(word(b) == 0x00010002);
    CHECK(a.call("cellSyncMutexTryLock", kMutex) == kBusy);
    REQUIRE(a.call("cellSyncMutexUnlock", kMutex) == 0);
    CHECK(word(b) == 0x00020002);

    // Unlock doesn't check that the mutex is held (there is no PERM error): on a free mutex it counts a release that
    // no ticket matches, and TryLock then reports BUSY.
    REQUIRE(a.call("cellSyncMutexUnlock", kMutex) == 0);
    CHECK(word(b) == 0x00030002);
    CHECK(a.call("cellSyncMutexTryLock", kMutex) == kBusy);

    // Both counters wrap at 16 bits.
    ps_write32(b, kMutex, 0xFFFFFFFF);
    REQUIRE(a.call("cellSyncMutexTryLock", kMutex) == 0);
    CHECK(word(b) == 0xFFFF0000);
    CHECK(a.call("cellSyncMutexTryLock", kMutex) == kBusy);
    REQUIRE(a.call("cellSyncMutexUnlock", kMutex) == 0);
    CHECK(word(b) == 0x00000000);
    REQUIRE(a.call("cellSyncMutexTryLock", kMutex) == 0);
    CHECK(word(b) == 0x00000001);
}

TEST_CASE("cellSync mutex updates are conditional stores through the reservation system", "[hle][sync]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    PPUContext ppu; // guest code on another PPU thread, with lwarx/stwcx. on the mutex
    ps_ppu_context_init(&ppu);
    REQUIRE(a.call("cellSyncMutexInitialize", kMutex) == 0);
    REQUIRE(a.call("cellSyncMutexTryLock", kMutex) == 0);

    // A TryLock that finds the mutex held writes nothing: a reservation taken before it still succeeds (here with the
    // holder's Unlock), and nothing waiting for the line to change wakes.
    u32 w = ps_ppu_lwarx(&ppu, b, kMutex);
    CHECK(w == 0x00000001);
    const u64 version = ps_res_version(kMutex);
    CHECK(a.call("cellSyncMutexTryLock", kMutex) == kBusy);
    CHECK_FALSE(ps_res_wait_change(kMutex, version, 1'000'000));
    CHECK(ps_ppu_stwcx(&ppu, b, kMutex, w + 0x10000) == 1);
    CHECK(word(b) == 0x00010001);

    // A TryLock, Unlock or Initialize that writes is a store to the line: a reservation taken before it fails, and
    // whatever waits for the line wakes. The second Initialize leaves the word as it is (0) and still does both, as a
    // store by another processor clears the reservations on its line on a PS3.
    for (const char* fn :
        {"cellSyncMutexTryLock", "cellSyncMutexUnlock", "cellSyncMutexInitialize", "cellSyncMutexInitialize"}) {
        INFO(fn);
        w = ps_ppu_lwarx(&ppu, b, kMutex);
        const u64 before = ps_res_version(kMutex);
        REQUIRE(a.call(fn, kMutex) == 0);
        CHECK(ps_res_wait_change(kMutex, before, 1'000'000));
        CHECK(ps_ppu_stwcx(&ppu, b, kMutex, w) == 0);
    }
    CHECK(word(b) == 0);
}

TEST_CASE("cellSync mutex: hammered by HLE TryLock and Unlock, a PPU's lwarx/stwcx. and an SPU taking tickets, it "
          "never has two holders", "[hle][sync]")
{
    constexpr u32 kHleThreads = 4, kRounds = 20000;
    constexpr u32 kHolds = (kHleThreads + 2) * kRounds;
    constexpr u32 kCount = kMutex - 4; // a plain counter in the mutex's 8-byte word, which every holder bumps
    constexpr u32 kStart = 0xFFF0;     // both counters wrap after 16 holds

    Fixture f;
    u8* b = f.base();
    const hle::Function& try_lock = GuestThread::find("cellSyncMutexTryLock");
    const hle::Function& unlock = GuestThread::find("cellSyncMutexUnlock");
    {
        GuestThread a(f, 0, "init");
        REQUIRE(a.call("cellSyncMutexInitialize", kMutex) == 0);
    }
    ps_write32(b, kMutex, (kStart << 16) | kStart);
    ps_write32(b, kCount, 0);

    std::atomic<u32> ready{0}, inside{0}, overlaps{0}, errors{0};
    std::atomic<u64> busy{0};
    std::atomic<bool> stop{false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    // All threads start together, so they contend from the first round.
    auto start = [&] {
        ready++;
        while (ready.load() < kHleThreads + 2) std::this_thread::yield();
    };
    // Each look at a held mutex: false once time is up (a lost Unlock would leave everyone waiting), so nothing hangs.
    // Waiters spin, so they race for the mutex the moment it is free, and yield now and then for a holder that lost
    // its core.
    auto keep_waiting = [&] {
        if (++busy % 64 == 0) {
            if (stop || std::chrono::steady_clock::now() > deadline) {
                stop = true;
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    };
    // The critical section: alone, and a plain read-modify-write of the counter, which loses counts if it isn't. The
    // counter is read again a few times first: another holder could change it meanwhile.
    auto hold = [&] {
        if (inside.fetch_add(1) != 0) overlaps++;
        const u32 count = ps_read32(b, kCount);
        for (u32 k = 0; k < 16; k++) {
            if (ps_read32(b, kCount) != count) overlaps++;
        }
        ps_write32(b, kCount, count + 1);
        inside.fetch_sub(1);
    };

    std::vector<std::thread> threads;
    // Guest threads calling the HLE functions (GuestThread::call without its Catch2 checks, which aren't thread-safe).
    for (u32 i = 0; i < kHleThreads; i++) {
        threads.emplace_back([&, i] {
            GuestThread g(f, 1 + i, "hle");
            auto call = [&](const hle::Function& fn) {
                g.ctx.gpr[3] = kMutex;
                fn.fn(g.ctx, b);
                return g.ctx.gpr[3];
            };
            start();
            for (u32 n = 0; n < kRounds; n++) {
                u64 rc;
                while ((rc = call(try_lock)) == kBusy) {
                    if (!keep_waiting()) return;
                }
                if (rc != 0) {
                    errors++;
                    return;
                }
                hold();
                if (call(unlock) != 0) {
                    errors++;
                    return;
                }
            }
        });
    }
    // Guest code on a PPU: the same TryLock and Unlock as lwarx/stwcx. loops.
    threads.emplace_back([&] {
        PPUContext ppu;
        ps_ppu_context_init(&ppu);
        start();
        for (u32 n = 0; n < kRounds; n++) {
            for (;;) {
                const u32 w = ps_ppu_lwarx(&ppu, b, kMutex);
                if (w >> 16 != (w & 0xFFFF)) {
                    if (!keep_waiting()) return;
                } else if (ps_ppu_stwcx(&ppu, b, kMutex, (w & 0xFFFF0000) | ((w + 1) & 0xFFFF))) {
                    break;
                }
            }
            hold();
            for (;;) {
                const u32 w = ps_ppu_lwarx(&ppu, b, kMutex);
                if (ps_ppu_stwcx(&ppu, b, kMutex, w + 0x10000)) break; // rel + 1; a carry leaves the word
            }
        }
    });
    // An SPU locking as the SPU side of the library does: a ticket (acq + 1) with GETLLAR/PUTLLC, then GETLLAR until rel
    // reaches it; Unlock (rel + 1) with GETLLAR/PUTLLC.
    threads.emplace_back([&] {
        constexpr u32 off = kMutex & (PS_RES_LINE_SIZE - 1);
        alignas(16) u8 line[PS_RES_LINE_SIZE];
        alignas(16) u8 data[PS_RES_LINE_SIZE];
        // Adds 1 to the u16 at `field` of the mutex and returns its old value; again until PUTLLC succeeds.
        auto increment = [&](u32 field) {
            for (;;) {
                const u64 version = ps_res_acquire(b, kMutex, line);
                std::memcpy(data, line, sizeof data);
                const u16 old = ps_read16(data, off + field);
                ps_write16(data, off + field, static_cast<u16>(old + 1));
                if (ps_res_store_line(b, kMutex, version, line, data)) return old;
            }
        };
        start();
        for (u32 n = 0; n < kRounds; n++) {
            const u16 ticket = increment(kAcq);
            for (;;) {
                ps_res_acquire(b, kMutex, line);
                if (ps_read16(line, off + kRel) == ticket) break;
                if (!keep_waiting()) return;
            }
            hold();
            increment(kRel);
        }
    });
    for (std::thread& t : threads) t.join();

    INFO("looks at a held mutex: " << busy.load());
    CHECK(errors.load() == 0u);
    CHECK(overlaps.load() == 0u);
    REQUIRE_FALSE(stop.load()); // every thread finished its rounds (two holders lose a ticket, and everyone waits)
    CHECK(ps_read32(b, kCount) == kHolds);
    const u32 end = (kStart + kHolds) & 0xFFFF;
    CHECK(word(b) == ((end << 16) | end));
}
