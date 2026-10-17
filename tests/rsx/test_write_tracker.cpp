// Write tracking for GPU copies of guest memory: hash mode, protection mode through the fault hook (called directly
// and through a real SIGBUS/SIGSEGV round trip), notify_write, unwatch.

#include <catch_amalgamated.hpp>

#include <atomic>
#include <csignal>
#include <thread>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/rsx/write_tracker.hpp"

using namespace pspocket;
using namespace pspocket::rsx;

TEST_CASE("hash mode detects content changes")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    const u32 base = PS_MEM_RSX_LOCAL_BASE;
    REQUIRE(mem.commit(base, 0x10000));
    WriteTracker tracker(mem);
    CHECK_FALSE(tracker.protection());

    auto w = tracker.watch(base + 0x100, 0x200);
    CHECK_FALSE(tracker.is_dirty(w));
    mem.write<u32>(base + 0x400, 1); // outside the range
    CHECK_FALSE(tracker.is_dirty(w));
    mem.write<u32>(base + 0x200, 0x12345678);
    CHECK(tracker.is_dirty(w));

    auto w2 = tracker.watch(base + 0x100, 0x200);
    CHECK_FALSE(tracker.is_dirty(w2));
    tracker.notify_write(base + 0x2f0, 4); // a write the tracker could not see
    CHECK(tracker.is_dirty(w2));
    tracker.unwatch(w);
    tracker.unwatch(w2);
}

TEST_CASE("protection mode: the fault hook marks pages dirty and makes them writable")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    const u32 page = static_cast<u32>(GuestMemory::host_page_size());
    const u32 base = PS_MEM_RSX_LOCAL_BASE;
    REQUIRE(mem.commit(base, 4 * page));
    WriteTracker tracker(mem);
    tracker.set_protection(true);

    auto w = tracker.watch(base + page + 16, 2 * page); // touches pages 1, 2 and 3
    CHECK_FALSE(tracker.is_dirty(w));
    const u64 gen = tracker.page_generation(base + 2 * page);

    // Not a tracked page: not ours.
    CHECK_FALSE(tracker.handle_fault(mem.host(base)));
    // Outside guest memory entirely.
    int local = 0;
    CHECK_FALSE(tracker.handle_fault(&local));

    // A fault on page 2 resolves it: generation bumped, the watch is dirty.
    CHECK(tracker.handle_fault(mem.host(base + 2 * page + 8)));
    CHECK(tracker.page_generation(base + 2 * page) == gen + 1);
    CHECK(tracker.is_dirty(w));
    CHECK(tracker.faults_handled() == 1);
    // A racing second fault on the same page retries the store (page already resolved).
    CHECK(tracker.handle_fault(mem.host(base + 2 * page + 64)));
    CHECK(tracker.faults_handled() == 1);

    // The page is writable again; the other pages are still protected and can be re-armed by a new watch.
    mem.write<u32>(base + 2 * page + 8, 7);
    auto w2 = tracker.watch(base + page + 16, 2 * page);
    CHECK_FALSE(tracker.is_dirty(w2));
    tracker.unwatch(w);
    tracker.unwatch(w2);
    // Unwatched pages are writable: a plain store must not fault.
    mem.write<u32>(base + page + 16, 1);
    mem.write<u32>(base + 3 * page, 1);

    // A store that faulted while page 1 was protected, handled after the unwatch made it writable: retried, not a
    // crash (D-144). A fault that keeps coming back there is not ours after all.
    CHECK(tracker.handle_fault(mem.host(base + page + 16)));
    u32 retries = 1;
    while (retries < 100000 && tracker.handle_fault(mem.host(base + page + 16))) retries++;
    CHECK(retries < 100000);
    // Watched and released again: retried again.
    auto w3 = tracker.watch(base + page + 16, 4);
    tracker.unwatch(w3);
    CHECK(tracker.handle_fault(mem.host(base + page + 16)));
    // Freed since: never retried (no access to it can succeed).
    REQUIRE(mem.decommit(base + page, page));
    CHECK_FALSE(tracker.handle_fault(mem.host(base + page + 16)));
}

namespace {

struct sigaction g_prev_segv {}, g_prev_bus {};
std::atomic<int> g_unexpected{0};

void fault_handler(int sig, siginfo_t* info, void* ctx)
{
    if (ps_rsx_handle_write_fault(info->si_addr)) return; // resolved: the store retries
    g_unexpected++;
    struct sigaction& prev = sig == SIGBUS ? g_prev_bus : g_prev_segv;
    if (prev.sa_flags & SA_SIGINFO) {
        prev.sa_sigaction(sig, info, ctx);
    } else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN) {
        prev.sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

} // namespace

TEST_CASE("protection mode: a real guest store faults once, is resolved and completes")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    const u32 page = static_cast<u32>(GuestMemory::host_page_size());
    const u32 base = PS_MEM_RSX_LOCAL_BASE + 0x100000;
    REQUIRE(mem.commit(base, 2 * page));
    WriteTracker tracker(mem);
    tracker.set_protection(true);
    tracker.install();

    struct sigaction sa {};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    sigaction(SIGBUS, &sa, &g_prev_bus);

    auto w = tracker.watch(base, 2 * page);
    CHECK_FALSE(tracker.is_dirty(w));
    ps_write32(mem.base(), base + 100, 0xdeadbeef); // faults, handled, retried
    ps_write32(mem.base(), base + 200, 0x01020304); // same page: no second fault
    ps_write32(mem.base(), base + page + 4, 5);      // second page: one more fault

    sigaction(SIGSEGV, &g_prev_segv, nullptr);
    sigaction(SIGBUS, &g_prev_bus, nullptr);

    CHECK(g_unexpected.load() == 0);
    CHECK(tracker.faults_handled() == 2);
    CHECK(tracker.is_dirty(w));
    CHECK(ps_read32(mem.base(), base + 100) == 0xdeadbeef);
    CHECK(ps_read32(mem.base(), base + 200) == 0x01020304);
    CHECK(ps_read32(mem.base(), base + page + 4) == 5);
    tracker.unwatch(w);
    tracker.uninstall();
    CHECK_FALSE(ps_rsx_handle_write_fault(mem.host(base)));
}

TEST_CASE("protection mode: check() reads nothing while untouched, keeps a watch a store beside it dirtied, and catches "
          "a store into it (D-134)")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    const u32 page = static_cast<u32>(GuestMemory::host_page_size());
    const u32 base = PS_MEM_RSX_LOCAL_BASE + 0x200000;
    REQUIRE(mem.commit(base, 2 * page));
    WriteTracker tracker(mem);
    tracker.set_protection(true);
    tracker.install();
    struct sigaction sa {};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    sigaction(SIGBUS, &sa, &g_prev_bus);

    auto w = tracker.watch(base + 0x100, 0x200); // a texture in the first page
    bool hashed = true;
    CHECK_FALSE(tracker.check(w, &hashed));
    CHECK_FALSE(hashed); // nothing written: nothing read

    // A store elsewhere in the page: a fault, then one read that finds the texture's bytes unchanged; the watch stays
    // and its page is read-only again, so the next store faults too.
    ps_write32(mem.base(), base + 0x800, 1);
    CHECK(tracker.faults_handled() == 1);
    CHECK_FALSE(tracker.check(w, &hashed));
    CHECK(hashed);
    CHECK_FALSE(tracker.check(w, &hashed));
    CHECK_FALSE(hashed);
    ps_write32(mem.base(), base + 0x900, 2);
    CHECK(tracker.faults_handled() == 2);

    // A store into the texture: dirty.
    ps_write32(mem.base(), base + 0x180, 3);
    CHECK(tracker.check(w, &hashed));
    CHECK(hashed);

    sigaction(SIGSEGV, &g_prev_segv, nullptr);
    sigaction(SIGBUS, &g_prev_bus, nullptr);
    CHECK(g_unexpected.load() == 0);
    tracker.unwatch(w);
    tracker.uninstall();
}

// Regression (D-144): a fault on a page (another thread storing beside a texture) made it writable again with an
// mprotect that could land after check() had made the page read-only again: the page stayed writable while the
// tracker took it for protected, and later stores into the texture went unseen (GTA V drew stale textures, the screen
// flickering with other textures' colours). Other threads store beside the texture all the time; every store into it
// must be seen by the next check().
TEST_CASE("protection mode: a store into a watched range is always seen while other threads fault on its page (D-144)")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    const u32 page = static_cast<u32>(GuestMemory::host_page_size());
    const u32 base = PS_MEM_RSX_LOCAL_BASE + 0x300000;
    REQUIRE(mem.commit(base, page));
    WriteTracker tracker(mem);
    tracker.set_protection(true);
    tracker.install();
    struct sigaction sa {};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    sigaction(SIGBUS, &sa, &g_prev_bus);

    std::atomic<bool> stop{false};
    std::vector<std::thread> noise;
    for (u32 k = 0; k < 3; k++) {
        noise.emplace_back([&, k] {
            u32 v = 0;
            while (!stop.load(std::memory_order_relaxed)) ps_write32(mem.base(), base + 0x800 + 64 * k, ++v);
        });
    }
    auto w = tracker.watch(base + 0x100, 0x100);
    u32 missed = 0;
    for (u32 i = 1; i <= 50000; i++) {
        tracker.check(w);                                  // may make the page read-only again
        ps_write32(mem.base(), base + 0x180, 0x5A000000 + i); // a new value in the range: must be seen
        if (!tracker.check(w)) missed++;
        tracker.unwatch(w); // as the texture cache does with a dirty texture: decoded again, watched again
        w = tracker.watch(base + 0x100, 0x100);
    }
    stop = true;
    for (auto& t : noise) t.join();
    sigaction(SIGSEGV, &g_prev_segv, nullptr);
    sigaction(SIGBUS, &g_prev_bus, nullptr);
    CHECK(missed == 0);
    CHECK(g_unexpected.load() == 0);
    tracker.unwatch(w);
    tracker.uninstall();
}

TEST_CASE("read watching: another thread's read of an armed page is a hit, the owner's is not, and the page keeps the "
          "write tracking's protection (D-1059)")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    const u32 page = static_cast<u32>(GuestMemory::host_page_size());
    const u32 base = PS_MEM_RSX_LOCAL_BASE + 0x200000;
    REQUIRE(mem.commit(base, 4 * page));
    mem.write<u32>(base, 0x11);
    mem.write<u32>(base + page, 0x22);
    mem.write<u32>(base + 2 * page, 0x33);
    WriteTracker tracker(mem);
    tracker.set_protection(true);
    tracker.install();
    struct sigaction sa {};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    sigaction(SIGBUS, &sa, &g_prev_bus);

    auto w = tracker.watch(base + page, 4); // page 1 also write-watched: read-only once its read is seen
    tracker.set_owner_thread();
    tracker.watch_reads({{base, 3 * page}}); // pages 0-2
    u32 seen = 0;
    std::thread other([&] { seen = mem.read<u32>(base + page) + mem.read<u32>(base + 2 * page); });
    other.join();
    CHECK(seen == 0x55);
    CHECK(mem.read<u32>(base) == 0x11); // the owner's: not a hit
    u64 total = 0;
    const auto hits = tracker.take_read_hits(&total);
    CHECK(total == 2);
    REQUIRE(hits.size() == 2);
    CHECK(((hits[0].addr == base + page && hits[1].addr == base + 2 * page) ||
           (hits[1].addr == base + page && hits[0].addr == base + 2 * page)));
    // Page 1 is read-only again (its write watch): a store there is the write tracking's fault, the watch dirty.
    CHECK_FALSE(tracker.is_dirty(w));
    mem.write<u32>(base + page, 0x44);
    CHECK(tracker.is_dirty(w));
    // Armed again, then disarmed by a call without it: reads go through without a hit.
    tracker.watch_reads({{base + 3 * page, page}});
    std::thread again([&] { seen = mem.read<u32>(base + 2 * page); });
    again.join();
    tracker.take_read_hits(&total);
    CHECK(total == 0);
    tracker.watch_reads({});
    tracker.unwatch(w);
    sigaction(SIGSEGV, &g_prev_segv, nullptr);
    sigaction(SIGBUS, &g_prev_bus, nullptr);
    CHECK(g_unexpected.load() == 0);
    tracker.uninstall();
}
