// Litmus and stress tests for the shared reservation system (pspocket/core/reservation.h).
//
// Every stress test is time-bounded: PSPOCKET_STRESS_SECONDS (default 0.3) per test, so CI stays short and a long
// soak is one environment variable away. Each test checks for zero lost updates: final memory equals the sum of
// the successful operations every thread counted.
//
// Plain guest stores in the mixed tests go through plain_store*(): the volatile accessors of be.h normally, relaxed
// atomics under ThreadSanitizer (the same single STR on ARM64; TSan would otherwise flag the deliberate mix of
// plain and atomic accesses that guest code performs).

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/reservation.h"

using namespace pspocket;

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PS_TEST_TSAN 1
#endif
#endif
#ifndef PS_TEST_TSAN
#define PS_TEST_TSAN 0
#endif

namespace {

constexpr u32 kArena = 0x10000000; // 8 MB of committed guest memory for every test
constexpr u64 kArenaSize = 8ull << 20;

double stress_seconds()
{
    if (const char* env = std::getenv("PSPOCKET_STRESS_SECONDS")) {
        const double v = std::atof(env);
        if (v > 0) return v;
    }
    return 0.3;
}

unsigned stress_threads()
{
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    return std::clamp(hw, 4u, 12u);
}

struct Arena {
    GuestMemory mem;
    Arena()
    {
        REQUIRE(mem.reserve());
        REQUIRE(mem.commit(kArena, kArenaSize));
    }
    u8* base() const { return mem.base(); }
};

void plain_store32(u8* base, u32 addr, u32 v)
{
#if PS_TEST_TSAN
    __atomic_store_n(reinterpret_cast<u32*>(base + addr), ps_bswap32(v), __ATOMIC_RELAXED);
#else
    ps_write32(base, addr, v);
#endif
}

u32 plain_load32(u8* base, u32 addr)
{
#if PS_TEST_TSAN
    return ps_bswap32(__atomic_load_n(reinterpret_cast<u32*>(base + addr), __ATOMIC_RELAXED));
#else
    return ps_read32(base, addr);
#endif
}

void plain_store64(u8* base, u32 addr, u64 v)
{
#if PS_TEST_TSAN
    __atomic_store_n(reinterpret_cast<u64*>(base + addr), ps_bswap64(v), __ATOMIC_RELAXED);
#else
    ps_write64(base, addr, v);
#endif
}

u64 plain_load64(u8* base, u32 addr)
{
#if PS_TEST_TSAN
    return ps_bswap64(__atomic_load_n(reinterpret_cast<u64*>(base + addr), __ATOMIC_RELAXED));
#else
    return ps_read64(base, addr);
#endif
}

u32 snap32(const u8* snap, u32 addr) { return ps_ld32(snap + (addr & (PS_RES_LINE_SIZE - 1))); }
u64 snap64(const u8* snap, u32 addr) { return ps_ld64(snap + (addr & (PS_RES_LINE_SIZE - 1))); }

// lwarx/stwcx. increment; returns the number of failed attempts.
u64 atomic_inc32(u8* base, u32 addr)
{
    alignas(16) u8 snap[PS_RES_LINE_SIZE];
    u64 failures = 0;
    for (;;) {
        const u64 v = ps_res_acquire(base, addr, snap);
        if (ps_res_store_conditional(base, addr, 4, snap32(snap, addr) + 1, v, snap)) return failures;
        failures++;
    }
}

u64 atomic_inc64(u8* base, u32 addr)
{
    alignas(16) u8 snap[PS_RES_LINE_SIZE];
    u64 failures = 0;
    for (;;) {
        const u64 v = ps_res_acquire(base, addr, snap);
        if (ps_res_store_conditional(base, addr, 8, snap64(snap, addr) + 1, v, snap)) return failures;
        failures++;
    }
}

// Compare-and-swap built from the primitives: true if the 64-bit word held `expected` and now holds `desired`.
bool cas64(u8* base, u32 addr, u64 expected, u64 desired)
{
    alignas(16) u8 snap[PS_RES_LINE_SIZE];
    for (;;) {
        const u64 v = ps_res_acquire(base, addr, snap);
        if (snap64(snap, addr) != expected) return false;
        if (ps_res_store_conditional(base, addr, 8, desired, v, snap)) return true;
    }
}

void store64(u8* base, u32 addr, u64 value)
{
    alignas(16) u8 snap[PS_RES_LINE_SIZE];
    for (;;) {
        const u64 v = ps_res_acquire(base, addr, snap);
        if (ps_res_store_conditional(base, addr, 8, value, v, snap)) return;
    }
}

u64 load64(u8* base, u32 addr)
{
    alignas(16) u8 snap[PS_RES_LINE_SIZE];
    ps_res_acquire(base, addr, snap);
    return snap64(snap, addr);
}

template <class Fn>
void run_threads(unsigned count, Fn&& fn)
{
    std::vector<std::thread> threads;
    threads.reserve(count);
    for (unsigned i = 0; i < count; i++) threads.emplace_back([&fn, i] { fn(i); });
    for (auto& t : threads) t.join();
}

struct Deadline {
    std::chrono::steady_clock::time_point end;
    explicit Deadline(double seconds)
        : end(std::chrono::steady_clock::now() +
              std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds)))
    {
    }
    bool passed() const { return std::chrono::steady_clock::now() >= end; }
};

} // namespace

TEST_CASE("reservation: single-thread semantics", "[reservation]")
{
    Arena a;
    u8* base = a.base();
    const u32 line = kArena + 0x1000;
    alignas(16) u8 snap[PS_RES_LINE_SIZE];

    ps_write32(base, line + 8, 0x11223344);
    u64 v = ps_res_acquire(base, line + 8, snap);
    CHECK((v & 1) == 0);
    CHECK(std::memcmp(snap, base + line, PS_RES_LINE_SIZE) == 0);
    CHECK(ps_res_version(line + 0x40) == v);

    // stwcx. writes big-endian and bumps the version by 2.
    REQUIRE(ps_res_store_conditional(base, line + 8, 4, 0xAABBCCDD, v, snap));
    CHECK(ps_read32(base, line + 8) == 0xAABBCCDD);
    CHECK(ps_read32(base, line + 12) == 0);
    CHECK(ps_res_version(line) == v + 2);

    // The old reservation is gone.
    CHECK_FALSE(ps_res_store_conditional(base, line + 8, 4, 1, v, snap));
    CHECK(ps_read32(base, line + 8) == 0xAABBCCDD);

    // Upper and lower halves of a doubleword, and stdcx.
    v = ps_res_acquire(base, line, snap);
    REQUIRE(ps_res_store_conditional(base, line + 12, 4, 0x55667788, v, snap));
    CHECK(ps_read64(base, line + 8) == 0xAABBCCDD55667788ull);
    v = ps_res_acquire(base, line, snap);
    REQUIRE(ps_res_store_conditional(base, line + 0x78, 8, 0x0102030405060708ull, v, snap));
    CHECK(ps_read64(base, line + 0x78) == 0x0102030405060708ull);

    // A plain store anywhere in the line (no version bump) fails the reservation through the byte comparison.
    v = ps_res_acquire(base, line, snap);
    ps_write8(base, line + 0x7F, 0x99);
    CHECK(ps_res_version(line) == v);
    CHECK_FALSE(ps_res_store_conditional(base, line + 8, 4, 0, v, snap));
    CHECK(ps_read32(base, line + 8) == 0xAABBCCDD);
    CHECK(ps_res_version(line) == v); // a failed store leaves the version alone

    // Misaligned or odd sizes are refused.
    v = ps_res_acquire(base, line, snap);
    CHECK_FALSE(ps_res_store_conditional(base, line + 2, 4, 0, v, snap));
    CHECK_FALSE(ps_res_store_conditional(base, line + 4, 8, 0, v, snap));
    CHECK_FALSE(ps_res_store_conditional(base, line, 2, 0, v, snap));

    // PUTLLC: whole line, conditional on version and bytes.
    alignas(16) u8 data[PS_RES_LINE_SIZE];
    for (u32 i = 0; i < PS_RES_LINE_SIZE; i++) data[i] = static_cast<u8>(i * 7);
    v = ps_res_acquire(base, line, snap);
    REQUIRE(ps_res_store_line(base, line + 0x33, v, snap, data));
    CHECK(std::memcmp(base + line, data, PS_RES_LINE_SIZE) == 0);
    CHECK_FALSE(ps_res_store_line(base, line, v, snap, snap)); // stale version
    const u64 v2 = ps_res_acquire(base, line, snap);
    CHECK(v2 == v + 2);
    ps_write32(base, line + 0x40, 0xDEADBEEF);
    CHECK_FALSE(ps_res_store_line(base, line, v2, snap, data)); // stale bytes
    CHECK(ps_read32(base, line + 0x40) == 0xDEADBEEF);

    // PUTLLUC: unconditional, bumps the version.
    std::memset(data, 0x5A, sizeof(data));
    ps_res_store_line_unconditional(base, line, data);
    CHECK(std::memcmp(base + line, data, PS_RES_LINE_SIZE) == 0);
    CHECK(ps_res_version(line) == v2 + 2);

    // notify_write bumps every line the range touches, and only those (unless they share a slot).
    const u64 l0 = ps_res_version(line + 0x80);
    const u64 l1 = ps_res_version(line + 0x100);
    const u64 l2 = ps_res_version(line + 0x180);
    const u64 l3 = ps_res_version(line + 0x200);
    const u64 before = ps_res_acquire(base, line + 0x80, snap);
    ps_res_notify_write(line + 0xF0, 0x100); // 0x...F0 .. 0x...1F0: lines 0x80, 0x100, 0x180
    CHECK(ps_res_version(line + 0x80) >= l0 + 2);
    CHECK(ps_res_version(line + 0x100) >= l1 + 2);
    CHECK(ps_res_version(line + 0x180) >= l2 + 2);
    CHECK(ps_res_version(line + 0x200) <= l3 + 6); // untouched unless it shares a slot with one of them
    CHECK_FALSE(ps_res_store_conditional(base, line + 0x80, 4, 1, before, snap));
}

TEST_CASE("reservation: 32-bit lwarx/stwcx. counters lose no updates", "[reservation][stress]")
{
    Arena a;
    u8* base = a.base();
    const unsigned threads = stress_threads();
    // Four counters in one line (contention plus neighbours), four in separate lines.
    std::array<u32, 8> counters{};
    for (u32 i = 0; i < 4; i++) counters[i] = kArena + 0x2000 + i * 4;
    for (u32 i = 0; i < 4; i++) counters[4 + i] = kArena + 0x3000 + i * 0x80 + (i & 1) * 4;

    std::vector<std::array<u64, 8>> done(threads);
    std::atomic<u64> failures{0};
    const Deadline deadline(stress_seconds());
    run_threads(threads, [&](unsigned t) {
        u64 fail = 0;
        u32 i = t;
        while (!deadline.passed()) {
            for (int k = 0; k < 64; k++, i = i * 1664525u + 1013904223u) {
                const u32 c = (i >> 16) & 7;
                fail += atomic_inc32(base, counters[c]);
                done[t][c]++;
            }
        }
        failures += fail;
    });

    u64 total = 0;
    for (u32 c = 0; c < 8; c++) {
        u64 expected = 0;
        for (unsigned t = 0; t < threads; t++) expected += done[t][c];
        total += expected;
        CHECK(ps_read32(base, counters[c]) == static_cast<u32>(expected));
    }
    CHECK(total > 0);
    INFO("increments " << total << ", failed attempts " << failures.load());
}

TEST_CASE("reservation: 64-bit ldarx/stdcx. counters lose no updates", "[reservation][stress]")
{
    Arena a;
    u8* base = a.base();
    const unsigned threads = stress_threads();
    const u32 c0 = kArena + 0x4000;      // shared line, word 0
    const u32 c1 = kArena + 0x4000 + 64; // shared line, word 8
    const u32 c2 = kArena + 0x4800;      // own line
    const u64 start = 0xFFFFFFF0ull;     // crosses the 32-bit boundary
    for (u32 c : {c0, c1, c2}) ps_write64(base, c, start);

    std::vector<std::array<u64, 3>> done(threads);
    const Deadline deadline(stress_seconds());
    run_threads(threads, [&](unsigned t) {
        const u32 cs[3] = {c0, c1, c2};
        u32 k = t;
        while (!deadline.passed()) {
            for (int n = 0; n < 64; n++, k++) {
                atomic_inc64(base, cs[k % 3]);
                done[t][k % 3]++;
            }
        }
    });

    const u32 cs[3] = {c0, c1, c2};
    for (u32 c = 0; c < 3; c++) {
        u64 expected = start;
        for (unsigned t = 0; t < threads; t++) expected += done[t][c];
        CHECK(ps_read64(base, cs[c]) == expected);
    }
}

TEST_CASE("reservation: GETLLAR/PUTLLC line updates are atomic and never torn", "[reservation][stress]")
{
    Arena a;
    u8* base = a.base();
    const unsigned threads = stress_threads();
    const u32 line = kArena + 0x5000;
    const u32 uniform = kArena + 0x5080; // only ever written whole, by PUTLLUC: every snapshot must be uniform

    std::vector<u64> done(threads);
    std::atomic<u64> torn{0};
    const Deadline deadline(stress_seconds());
    run_threads(threads, [&](unsigned t) {
        alignas(16) u8 snap[PS_RES_LINE_SIZE];
        alignas(16) u8 next[PS_RES_LINE_SIZE];
        u64 count = 0;
        while (!deadline.passed()) {
            if (t == 0) {
                // One PUTLLUC writer next to the PUTLLC traffic.
                ps_res_acquire(base, uniform, snap);
                const u64 w = ps_ld64(snap) + 1;
                for (u32 i = 0; i < PS_RES_LINE_SIZE; i += 8) ps_st64(next + i, w);
                ps_res_store_line_unconditional(base, uniform, next);
                continue;
            }
            const u64 v = ps_res_acquire(base, line, snap);
            const u64 w = ps_ld64(snap);
            for (u32 i = 8; i < PS_RES_LINE_SIZE; i += 8) {
                if (ps_ld64(snap + i) != w) torn++;
            }
            for (u32 i = 0; i < PS_RES_LINE_SIZE; i += 8) ps_st64(next + i, w + 1);
            if (ps_res_store_line(base, line, v, snap, next)) count++;

            ps_res_acquire(base, uniform, snap);
            for (u32 i = 8; i < PS_RES_LINE_SIZE; i += 8) {
                if (ps_ld64(snap + i) != ps_ld64(snap)) torn++;
            }
        }
        done[t] = count;
    });

    u64 expected = 0;
    for (u64 d : done) expected += d;
    CHECK(torn.load() == 0);
    CHECK(expected > 0);
    for (u32 i = 0; i < PS_RES_LINE_SIZE; i += 8) CHECK(ps_read64(base, line + i) == expected);
}

TEST_CASE("reservation: a line snapshot never shows a plain writer's last store without the ones before it (D-130)",
    "[reservation][stress]")
{
    // The PPU appends job chain commands with plain stores: the new commands, a release fence, then the word that
    // makes them visible. A snapshot (GETLLAR, the job chain's command fetch) read while it writes must not hold that
    // word's new value next to older values of the words written before it, whichever end of the line it is at.
    Arena a;
    u8* base = a.base();
    for (const bool flag_last : {true, false}) {
        INFO((flag_last ? "the flag after the data" : "the flag before the data"));
        const u32 line = kArena + (flag_last ? 0x6000 : 0x6080);
        const u32 flag = flag_last ? 15 : 0;
        std::atomic<bool> stop{false};
        std::atomic<u64> torn{0}, snaps{0};
        std::thread writer([&] {
            u64* words = reinterpret_cast<u64*>(base + line);
            for (u64 i = 1; !stop.load(std::memory_order_relaxed); i++) {
                for (u32 w = 0; w < 16; w++)
                    if (w != flag) __atomic_store_n(&words[w], i, __ATOMIC_RELAXED);
                std::atomic_thread_fence(std::memory_order_release); // lwsync
                __atomic_store_n(&words[flag], i, __ATOMIC_RELAXED);
            }
        });
        alignas(16) u8 snap[PS_RES_LINE_SIZE];
        const Deadline deadline(std::min(stress_seconds(), 0.5));
        while (!deadline.passed()) {
            ps_res_acquire(base, line, snap);
            u64 w[16];
            std::memcpy(w, snap, sizeof w);
            for (u32 k = 0; k < 16; k++)
                if (w[k] < w[flag]) torn++;
            snaps++;
        }
        stop = true;
        writer.join();
        CHECK(snaps.load() > 0);
        CHECK(torn.load() == 0);
    }
}

TEST_CASE("reservation: lines that share a version slot only cause retries", "[reservation][stress]")
{
    Arena a;
    u8* base = a.base();
    // Find two guest lines that share a version slot: a store to one moves the other's version.
    const u32 region = kArena + 0x100000;
    const u32 region_lines = 0x8000;
    alignas(16) u8 snap[PS_RES_LINE_SIZE];
    u32 la = 0, lb = 0;
    std::vector<u64> before(region_lines);
    for (u32 attempt = 0; attempt < 64 && lb == 0; attempt++) {
        const u32 candidate = kArena + 0x80 * attempt;
        for (u32 i = 0; i < region_lines; i++) before[i] = ps_res_version(region + i * 0x80);
        const u64 v = ps_res_acquire(base, candidate, snap);
        REQUIRE(ps_res_store_conditional(base, candidate, 4, 1, v, snap));
        for (u32 i = 0; i < region_lines; i++) {
            if (ps_res_version(region + i * 0x80) != before[i]) {
                la = candidate;
                lb = region + i * 0x80;
                break;
            }
        }
    }
    REQUIRE(lb != 0);

    // A store to one line fails a reservation on the other (spurious, allowed) ...
    u64 v = ps_res_acquire(base, lb, snap);
    atomic_inc32(base, la);
    CHECK_FALSE(ps_res_store_conditional(base, lb, 4, 7, v, snap));

    // ... but concurrent updates to both lose nothing.
    ps_write32(base, la, 0);
    ps_write32(base, lb, 0);
    const unsigned threads = stress_threads();
    std::vector<std::array<u64, 2>> done(threads);
    const Deadline deadline(stress_seconds());
    run_threads(threads, [&](unsigned t) {
        while (!deadline.passed()) {
            for (int k = 0; k < 64; k++) {
                const u32 which = (t + static_cast<u32>(k)) & 1;
                atomic_inc32(base, which ? lb : la);
                done[t][which]++;
            }
        }
    });
    u64 ea = 0, eb = 0;
    for (auto& d : done) {
        ea += d[0];
        eb += d[1];
    }
    CHECK(ps_read32(base, la) == static_cast<u32>(ea));
    CHECK(ps_read32(base, lb) == static_cast<u32>(eb));
}

TEST_CASE("reservation: a mutex built from the primitives guards a plain critical section", "[reservation][stress]")
{
    Arena a;
    u8* base = a.base();
    const unsigned threads = stress_threads();
    const u32 lock = kArena + 0x6000;  // 0 = free, else owner + 1
    const u32 data = kArena + 0x6100;  // two plain counters and an owner word, in another line
    std::atomic<u64> violations{0};
    std::vector<u64> done(threads);

    auto acquire_lock = [&](u32 me) {
        alignas(16) u8 snap[PS_RES_LINE_SIZE];
        for (;;) {
            const u64 v = ps_res_acquire(base, lock, snap);
            if (snap32(snap, lock) != 0) {
                // Sleep until the lock line changes instead of spinning (the reservation-lost event).
                ps_res_wait_change(lock, v, 2'000'000);
                continue;
            }
            if (ps_res_store_conditional(base, lock, 4, me + 1, v, snap)) return;
        }
    };
    auto release_lock = [&] {
        alignas(16) u8 snap[PS_RES_LINE_SIZE];
        for (;;) {
            const u64 v = ps_res_acquire(base, lock, snap);
            if (ps_res_store_conditional(base, lock, 4, 0, v, snap)) return;
        }
    };

    const Deadline deadline(stress_seconds());
    run_threads(threads, [&](unsigned t) {
        u64 count = 0;
        while (!deadline.passed()) {
            acquire_lock(t);
            ps_write32(base, data + 8, t);
            const u32 c0 = ps_read32(base, data);
            const u32 c1 = ps_read32(base, data + 4);
            if (c0 != c1) violations++;
            ps_write32(base, data, c0 + 1);
            ps_write32(base, data + 4, c1 + 1);
            if (ps_read32(base, data + 8) != t) violations++;
            release_lock();
            count++;
        }
        done[t] = count;
    });

    u64 expected = 0;
    for (u64 d : done) expected += d;
    CHECK(violations.load() == 0);
    CHECK(ps_read32(base, data) == static_cast<u32>(expected));
    CHECK(ps_read32(base, data + 4) == static_cast<u32>(expected));
    CHECK(ps_read32(base, lock) == 0);
}

TEST_CASE("reservation: an MPMC queue built from the primitives delivers every item once", "[reservation][stress]")
{
    // Vyukov's bounded queue: producers and consumers claim positions with CAS on tail / head, each slot carries a
    // sequence number that hands it from producer to consumer and back. All atomics are reservation loops.
    Arena a;
    u8* base = a.base();
    constexpr u32 kSlots = 64;
    const u32 head = kArena + 0x7000;
    const u32 tail = kArena + 0x7080;
    const u32 slots = kArena + 0x8000; // one line per slot: [seq u64][value u64]
    for (u32 i = 0; i < kSlots; i++) ps_write64(base, slots + i * 0x80, i);

    const unsigned threads = stress_threads();
    const unsigned producers = threads / 2;
    const unsigned consumers = threads - producers;
    std::vector<u64> produced(producers);
    std::vector<std::vector<u64>> consumed(consumers);
    std::atomic<unsigned> producers_left{producers};
    std::atomic<u64> total_produced{0};
    std::atomic<u64> total_consumed{0};

    auto push = [&](u64 value) -> bool {
        for (;;) {
            const u64 pos = load64(base, tail);
            const u32 slot = slots + static_cast<u32>(pos % kSlots) * 0x80;
            const u64 seq = load64(base, slot);
            const s64 dif = static_cast<s64>(seq - pos);
            if (dif == 0) {
                if (cas64(base, tail, pos, pos + 1)) {
                    plain_store64(base, slot + 8, value);
                    store64(base, slot, pos + 1);
                    return true;
                }
            } else if (dif < 0) {
                return false; // full
            }
        }
    };
    auto pop = [&](u64& value) -> bool {
        for (;;) {
            const u64 pos = load64(base, head);
            const u32 slot = slots + static_cast<u32>(pos % kSlots) * 0x80;
            const u64 seq = load64(base, slot);
            const s64 dif = static_cast<s64>(seq - (pos + 1));
            if (dif == 0) {
                if (cas64(base, head, pos, pos + 1)) {
                    value = plain_load64(base, slot + 8);
                    store64(base, slot, pos + kSlots);
                    return true;
                }
            } else if (dif < 0) {
                return false; // empty
            }
        }
    };

    const Deadline deadline(stress_seconds());
    run_threads(threads, [&](unsigned t) {
        if (t < producers) {
            u64 n = 0;
            while (!deadline.passed()) {
                const u64 value = (u64{t} << 40) | n;
                if (push(value)) {
                    n++;
                } else {
                    std::this_thread::yield();
                }
            }
            produced[t] = n;
            total_produced += n;
            producers_left--;
        } else {
            auto& mine = consumed[t - producers];
            u64 value = 0;
            for (;;) {
                if (pop(value)) {
                    mine.push_back(value);
                    total_consumed++;
                } else if (producers_left.load() == 0 && total_consumed.load() == total_produced.load()) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        }
    });

    std::vector<u64> all;
    for (auto& c : consumed) all.insert(all.end(), c.begin(), c.end());
    u64 expected = 0;
    for (u64 p : produced) expected += p;
    REQUIRE(all.size() == expected);
    std::sort(all.begin(), all.end());
    CHECK(std::adjacent_find(all.begin(), all.end()) == all.end()); // no duplicates
    // Every producer's sequence 0..n-1 arrived.
    std::vector<u64> seen(producers);
    for (u64 v : all) {
        const u64 p = v >> 40;
        REQUIRE(p < producers);
        seen[p]++;
        CHECK((v & ((u64{1} << 40) - 1)) < produced[p]);
    }
    for (unsigned p = 0; p < producers; p++) CHECK(seen[p] == produced[p]);
    CHECK(expected > 0);
}

TEST_CASE("reservation: plain stores mixed with conditional stores lose nothing", "[reservation][stress]")
{
    Arena a;
    u8* base = a.base();
    const u32 line = kArena + 0x9000;
    const u32 w_cond = line;          // stwcx. increments by several threads
    const u32 w_plain = line + 0x10;  // plain stores by one thread
    const u32 w_putllc = line + 0x20; // PUTLLC increments by one thread
    const u32 w_split = line + 0x30;  // upper half: stwcx. increments; lower half: plain stores, same doubleword

    std::atomic<bool> stop{false};
    std::atomic<u64> cond_done{0};
    u64 putllc_done = 0, split_done = 0;
    u32 plain_last = 0, split_plain_last = 0;
    const unsigned cond_threads = std::max(2u, stress_threads() / 2);

    std::vector<std::thread> pool;
    for (unsigned t = 0; t < cond_threads; t++) {
        pool.emplace_back([&] {
            u64 n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                atomic_inc32(base, w_cond);
                n++;
            }
            cond_done += n;
        });
    }
    pool.emplace_back([&] {
        u32 i = 0;
        while (!stop.load(std::memory_order_relaxed)) plain_store32(base, w_plain, ++i);
        plain_last = i;
    });
    pool.emplace_back([&] {
        u32 i = 0;
        while (!stop.load(std::memory_order_relaxed)) plain_store32(base, w_split + 4, ++i);
        split_plain_last = i;
    });
    pool.emplace_back([&] {
        alignas(16) u8 snap[PS_RES_LINE_SIZE];
        alignas(16) u8 next[PS_RES_LINE_SIZE];
        u64 n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const u64 v = ps_res_acquire(base, line, snap);
            std::memcpy(next, snap, sizeof(next));
            ps_st32(next + 0x20, ps_ld32(snap + 0x20) + 1);
            if (ps_res_store_line(base, line, v, snap, next)) n++;
        }
        putllc_done = n;
    });
    pool.emplace_back([&] {
        u64 n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            atomic_inc32(base, w_split);
            n++;
        }
        split_done = n;
    });

    std::this_thread::sleep_for(std::chrono::duration<double>(stress_seconds()));
    stop = true;
    for (auto& t : pool) t.join();

    CHECK(plain_load32(base, w_cond) == static_cast<u32>(cond_done.load()));
    CHECK(plain_load32(base, w_plain) == plain_last);
    CHECK(plain_load32(base, w_putllc) == static_cast<u32>(putllc_done));
    CHECK(plain_load32(base, w_split) == static_cast<u32>(split_done));
    CHECK(plain_load32(base, w_split + 4) == split_plain_last);
    CHECK(cond_done.load() > 0);
    CHECK(putllc_done > 0);
}

TEST_CASE("reservation: a waiter that read the version races DMA writes and is always woken (D-243)", "[reservation][stress]")
{
    // notify_write bumps only the slots someone reads a version of: a waiter that reads the version, then the line,
    // then waits, must never sleep through a write made around its read (the Dekker pairing of the watch bits).
    Arena a;
    u8* base = a.base();
    std::atomic<bool> stop{false};
    std::atomic<u64> waits{0}, slept_through{0};
    std::atomic<u32> generation{0};
    std::vector<std::thread> pool;
    for (u32 t = 0; t < 4; t++) {
        pool.emplace_back([&, t] {
            const u32 line = kArena + 0xC000 + t * 0x1000; // a fresh line each: its first reads race the writer's first
            alignas(16) u8 snap[PS_RES_LINE_SIZE];
            u32 seen = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const u64 v = ps_res_acquire(base, line, snap); // the version, then the bytes
                const u32 now = ps_ld32(snap);
                if (now != seen) { // it changed: done with this one
                    seen = now;
                    continue;
                }
                // Nothing new in the bytes read after the version: a write after them must change the version.
                waits++;
                if (!ps_res_wait_change(line, v, 200'000'000) && ps_read32(base, line) != seen) slept_through++;
            }
        });
    }
    std::thread writer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const u32 g = generation.fetch_add(1) + 1;
            for (u32 t = 0; t < 4; t++) {
                const u32 line = kArena + 0xC000 + t * 0x1000;
                __atomic_store_n(reinterpret_cast<u32*>(base + line), ps_bswap32(g), __ATOMIC_RELAXED); // a DMA PUT
                ps_res_notify_write(line, 4);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    std::this_thread::sleep_for(std::chrono::duration<double>(stress_seconds()));
    stop = true;
    writer.join();
    for (auto& t : pool) t.join();
    CHECK(waits.load() > 0);
    CHECK(slept_through.load() == 0); // a wait that timed out though the line had changed
}

TEST_CASE("reservation: wait_change wakes on stores and notify_write, and times out", "[reservation]")
{
    Arena a;
    u8* base = a.base();
    const u32 line = kArena + 0xA000;
    using clock = std::chrono::steady_clock;
    alignas(16) u8 snap[PS_RES_LINE_SIZE];

    SECTION("a stale version returns at once")
    {
        const u64 v = ps_res_acquire(base, line, snap);
        REQUIRE(ps_res_store_conditional(base, line, 4, 1, v, snap));
        CHECK(ps_res_wait_change(line, v, 0));
    }

    SECTION("timeout")
    {
        const u64 v = ps_res_version(line);
        const auto t0 = clock::now();
        CHECK_FALSE(ps_res_wait_change(line, v, 30'000'000));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
        CHECK(ms >= 29);
        CHECK(ms < 5000);
    }

    SECTION("a conditional store wakes every waiter")
    {
        const u64 v = ps_res_version(line);
        std::atomic<int> woke{0};
        std::vector<std::thread> waiters;
        for (int i = 0; i < 4; i++) {
            waiters.emplace_back([&] {
                if (ps_res_wait_change(line + 0x40, v, 0)) woke++;
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(woke.load() == 0);
        atomic_inc32(base, line + 4);
        for (auto& t : waiters) t.join();
        CHECK(woke.load() == 4);
    }

    SECTION("notify_write wakes a waiter and fails reservations")
    {
        const u64 v = ps_res_acquire(base, line, snap);
        std::atomic<bool> woke{false};
        std::thread waiter([&] { woke = ps_res_wait_change(line, v, 5'000'000'000ull); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ps_write32(base, line + 0x10, 42); // e.g. a DMA PUT
        ps_res_notify_write(line + 0x10, 4);
        waiter.join();
        CHECK(woke.load());
        CHECK_FALSE(ps_res_store_conditional(base, line, 4, 1, v, snap));
    }

    SECTION("a timed waiter wakes early on a store")
    {
        const u64 v = ps_res_version(line);
        std::atomic<bool> woke{false};
        const auto t0 = clock::now();
        std::thread waiter([&] { woke = ps_res_wait_change(line, v, 10'000'000'000ull); });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        atomic_inc32(base, line);
        waiter.join();
        CHECK(woke.load());
        CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count() < 5000);
    }
}

TEST_CASE("reservation: throughput", "[reservation][stress]")
{
    Arena a;
    u8* base = a.base();
    const unsigned threads = stress_threads();
    const double seconds = std::min(stress_seconds(), 1.0);

    auto measure = [&](const char* what, auto&& body) {
        std::atomic<u64> ops{0};
        const auto t0 = std::chrono::steady_clock::now();
        const Deadline deadline(seconds);
        run_threads(threads, [&](unsigned t) {
            u64 n = 0;
            while (!deadline.passed()) {
                for (int k = 0; k < 256; k++) body(t);
                n += 256;
            }
            ops += n;
        });
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double rate = static_cast<double>(ops.load()) / s;
        std::printf("reservation throughput, %u threads, %s: %.2f M successful stores/s\n", threads, what, rate / 1e6);
        CHECK(ops.load() > 0);
        return rate;
    };

    const u32 shared = kArena + 0xB000;
    measure("one contended 32-bit counter (stwcx.)", [&](unsigned) { atomic_inc32(base, shared); });
    measure("private lines (stwcx.)", [&](unsigned t) { atomic_inc32(base, kArena + 0xC000 + t * 0x80); });
    measure("one contended line (PUTLLC)", [&](unsigned) {
        alignas(16) u8 snap[PS_RES_LINE_SIZE];
        for (;;) {
            const u64 v = ps_res_acquire(base, shared + 0x80, snap);
            ps_st64(snap + 8, ps_ld64(snap + 8) + 1);
            alignas(16) u8 next[PS_RES_LINE_SIZE];
            std::memcpy(next, snap, sizeof(next));
            ps_st64(snap + 8, ps_ld64(snap + 8) - 1);
            if (ps_res_store_line(base, shared + 0x80, v, snap, next)) break;
        }
    });
}
