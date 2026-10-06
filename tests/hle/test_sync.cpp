// cellSync as RPCS3 has it (runtime/hle/src/cellSync.cpp), synthetic: no game data. Every function's checks in RPCS3's
// order (NULL_POINTER, ALIGN, INVAL; FAULT for memory that is not guest memory, D-042) and its effect on the structure,
// byte for byte; the Try forms answering BUSY (AGAIN for the lock-free queue) without writing; the blocking forms
// sleeping until another thread's update lets them go on (and not spinning meanwhile), and unwinding when the process
// ends. The mutex: the updates go through the reservation system (a TryLock that finds the mutex held leaves a guest
// reservation on the line valid and wakes nothing, every write fails such a reservation and wakes what waits for the
// line), and host threads hammer it together with guest-style users of the same word, a PPU thread with lwarx/stwcx. and
// an SPU with GETLLAR/PUTLLC, without two holders, with TryLock and with Lock. The barrier, rwm and queue across threads.
// The lock-free queue: libsre's layout and init 2, pointers handed out and completed out of order, a waiter signalled
// through a guest function, the lv2 event queue wait, and a PPU pushing (popping) against an SPU popping (pushing) with
// GETLLAR/PUTLLC.

#include "hle_test_util.hpp"

#include <time.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pspocket/core/reservation.h"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u64 kAgain = 0xFFFFFFFF80410101ull, kInval = 0xFFFFFFFF80410102ull, kNosys = 0xFFFFFFFF80410103ull,
              kPerm = 0xFFFFFFFF80410109ull, kBusy = 0xFFFFFFFF8041010Aull, kFault = 0xFFFFFFFF8041010Dull,
              kStat = 0xFFFFFFFF8041010Full, kAlign = 0xFFFFFFFF80410110ull, kNullPointer = 0xFFFFFFFF80410111ull;

constexpr u32 kLine = kData + 0x1000; // a 128-byte line: the mutex and data around it
constexpr u32 kMutex = kLine + 0x44;  // CellSyncMutex {be16 rel, be16 acq}, the second half of an 8-byte word
constexpr u32 kRel = 0, kAcq = 2;     // its fields
constexpr u32 kUnmapped = 0x0FFF0000; // in the main area, never committed by these tests (and 128-aligned)

// The mutex as one big-endian word: rel in the high half, acq in the low half.
u32 word(u8* b) { return ps_read32(b, kMutex); }

std::array<u8, PS_RES_LINE_SIZE> line_bytes(u8* b)
{
    std::array<u8, PS_RES_LINE_SIZE> out;
    std::memcpy(out.data(), b + kLine, out.size());
    return out;
}

// An HLE call from a host thread, without GuestThread::call's Catch2 checks (which aren't thread-safe).
u64 raw(GuestThread& g, const hle::Function& fn, std::initializer_list<u64> args)
{
    u32 r = 3;
    for (const u64 a : args) g.ctx.gpr[r++] = a;
    fn.fn(g.ctx, g.f.base());
    return g.ctx.gpr[3];
}

bool wait_for(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// CPU time of the calling host thread.
s64 thread_cpu_ns()
{
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<s64>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// One blocking HLE call on its own guest thread: `result` stays ~0 until it returns; `unwound` when the process ending
// unwound it; `cpu_ns` the CPU time the call took.
class Caller {
public:
    Caller(Fixture& f, u32 index, std::string_view fn, std::initializer_list<u64> args)
        : fn_(GuestThread::find(fn)), args_(args)
    {
        thread_ = std::thread([this, &f, index] {
            GuestThread g(f, index, "caller");
            lv2_thread = g.t;
            const s64 c0 = thread_cpu_ns();
            try {
                u32 r = 3;
                for (const u64 a : args_) g.ctx.gpr[r++] = a;
                fn_.fn(g.ctx, g.f.base());
                cpu_ns = thread_cpu_ns() - c0;
                result = g.ctx.gpr[3];
            } catch (const lv2::ThreadExit&) {
                unwound = true;
            }
            lv2_thread = nullptr;
        });
    }
    ~Caller() { join(); }
    Caller(const Caller&) = delete;
    Caller& operator=(const Caller&) = delete;

    bool done() const { return result.load() != ~0ull; }
    void join()
    {
        if (thread_.joinable()) thread_.join();
    }
    // Still blocked after a while.
    bool blocked()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return !done() && !unwound;
    }
    // Parked on an lv2 object of this kind (the lv2 event queue a lock-free queue call receives from).
    bool parked_on(lv2::WaitKind kind)
    {
        return wait_for([&] {
            const lv2::PpuThread* t = lv2_thread.load();
            return t && t->waiter.waiting_on().kind == kind;
        });
    }

    std::atomic<u64> result{~0ull};
    std::atomic<bool> unwound{false};
    std::atomic<s64> cpu_ns{-1};
    std::atomic<lv2::PpuThread*> lv2_thread{nullptr};

private:
    const hle::Function& fn_;
    const std::vector<u64> args_;
    std::thread thread_;
};

// An SPU's update of a line: GETLLAR, the change, PUTLLC, again until it lands. `fn(u8* line)` returns false to store
// nothing.
template <class Fn>
bool spu_update(u8* b, u32 addr, Fn fn)
{
    alignas(16) u8 line[PS_RES_LINE_SIZE];
    alignas(16) u8 data[PS_RES_LINE_SIZE];
    const u32 off = addr & (PS_RES_LINE_SIZE - 1);
    for (;;) {
        const u64 version = ps_res_acquire(b, addr, line);
        std::memcpy(data, line, sizeof data);
        if (!fn(data + off)) return false;
        if (ps_res_store_line(b, addr, version, line, data)) return true;
    }
}

// Guest code the HLE calls back into runs on the PPU interpreter while this lives.
struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

} // namespace

TEST_CASE("cellSync: every function RPCS3 implements, under its NID", "[hle][sync]")
{
    const std::pair<std::string_view, u32> expected[] = {
        {"cellSyncMutexInitialize", 0xA9072DEE},
        {"cellSyncMutexLock", 0x1BB675C2},
        {"cellSyncMutexTryLock", 0xD06918C4},
        {"cellSyncMutexUnlock", 0x91F2B7B0},
        {"cellSyncBarrierInitialize", 0x07254FDA},
        {"cellSyncBarrierNotify", 0xF06A6415},
        {"cellSyncBarrierTryNotify", 0x268EDD6D},
        {"cellSyncBarrierWait", 0x35F21355},
        {"cellSyncBarrierTryWait", 0x6C272124},
        {"cellSyncRwmInitialize", 0xFC48B03F},
        {"cellSyncRwmRead", 0xCECE771F},
        {"cellSyncRwmTryRead", 0xA6669751},
        {"cellSyncRwmWrite", 0xED773F5F},
        {"cellSyncRwmTryWrite", 0xBA5BEE48},
        {"cellSyncQueueInitialize", 0x3929948D},
        {"cellSyncQueuePush", 0x5AE841E5},
        {"cellSyncQueueTryPush", 0x705985CD},
        {"cellSyncQueuePop", 0x4DA6D7E0},
        {"cellSyncQueueTryPop", 0xA58DF87F},
        {"cellSyncQueuePeek", 0x48154C9B},
        {"cellSyncQueueTryPeek", 0x68AF923C},
        {"cellSyncQueueSize", 0x4DA349B2},
        {"cellSyncQueueClear", 0xA5362E73},
        {"cellSyncLFQueueInitialize", 0xAA355278},
        {"cellSyncLFQueueClear", 0x2AF0C515},
        {"cellSyncLFQueueSize", 0x167EA63E},
        {"cellSyncLFQueueDepth", 0xE18C273C},
        {"cellSyncLFQueueGetDirection", 0xD59AA307},
        {"cellSyncLFQueueGetEntrySize", 0x0C7CB9F7},
        {"_cellSyncLFQueueGetPushPointer", 0xE9BF2110},
        {"_cellSyncLFQueueGetPopPointer", 0x74C37666},
        {"_cellSyncLFQueueCompletePushPointer", 0x4E88C68D},
        {"_cellSyncLFQueueCompletePopPointer", 0xFE74E8E7},
        {"_cellSyncLFQueuePushBody", 0xBA5961CA},
        {"_cellSyncLFQueuePopBody", 0xE1BC7ADD},
        {"_cellSyncLFQueueGetSignalAddress", 0xAFF7627A},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const auto& [name, nid] : expected) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("cellSync", nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
    }
    // RPCS3's todo stubs stay unimplemented (an error, never a silent CELL_OK): _cellSyncLFQueueGetPushPointer2,
    // CompletePushPointer2, GetPopPointer2, CompletePopPointer2, AttachLv2EventQueue, DetachLv2EventQueue.
    for (const u32 nid : {0x6BB4EF9Du, 0x35BBDAD2u, 0x46356FE0u, 0x7A51DEEEu, 0x54FC2032u, 0x811D148Eu}) {
        CHECK(hle::registry().find("cellSync", nid) == nullptr);
    }
}

// ================================================================================================================ mutex

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

TEST_CASE("cellSyncMutexLock: RPCS3's checks; a ticket, then a sleep until Unlock reaches it", "[hle][sync]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    CHECK(a.call("cellSyncMutexLock", 0) == kNullPointer);
    CHECK(a.call("cellSyncMutexLock", kMutex + 2) == kAlign);
    CHECK(a.call("cellSyncMutexLock", kUnmapped) == kFault);

    REQUIRE(a.call("cellSyncMutexInitialize", kMutex) == 0);
    REQUIRE(a.call("cellSyncMutexLock", kMutex) == 0); // a free mutex: ticket 0, which rel 0 already reached
    CHECK(word(b) == 0x00000001);
    CHECK(a.call("cellSyncMutexTryLock", kMutex) == kBusy);

    // A second thread takes ticket 1 and waits for rel 1, asleep: hardly any CPU time while it waits 300 ms.
    Caller waiter(f, 1, "cellSyncMutexLock", {kMutex});
    REQUIRE(wait_for([&] { return word(b) == 0x00000002; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(waiter.done());
    REQUIRE(a.call("cellSyncMutexUnlock", kMutex) == 0); // its turn
    waiter.join();
    CHECK(waiter.result == 0);
    CHECK(waiter.cpu_ns < 50'000'000);
    CHECK(word(b) == 0x00010002);
    REQUIRE(a.call("cellSyncMutexUnlock", kMutex) == 0);
    CHECK(word(b) == 0x00020002);

    // Tickets wrap at 16 bits: ticket 0xFFFF goes as soon as rel is 0xFFFF.
    ps_write32(b, kMutex, 0xFFFFFFFF);
    REQUIRE(a.call("cellSyncMutexLock", kMutex) == 0);
    CHECK(word(b) == 0xFFFF0000);
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

namespace {

// Host threads hammer one mutex together with guest-style users of the same word, and it never has two holders: four
// threads calling the HLE functions (TryLock until it takes the mutex, or Lock), a PPU thread with lwarx/stwcx. (with
// TryLock: the same TryLock; with Lock: a ticket, then lwarx until rel reaches it) and an SPU taking tickets with
// GETLLAR/PUTLLC. Every holder bumps a plain counter beside the mutex.
void hammer_mutex(bool lock, u32 rounds)
{
    constexpr u32 kHleThreads = 4;
    const u32 holds = (kHleThreads + 2) * rounds;
    constexpr u32 kCount = kMutex - 4; // a plain counter in the mutex's 8-byte word, which every holder bumps
    constexpr u32 kStart = 0xFFF0;     // both counters wrap after 16 holds

    Fixture f;
    u8* b = f.base();
    const hle::Function& take = GuestThread::find(lock ? "cellSyncMutexLock" : "cellSyncMutexTryLock");
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
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

    std::vector<std::thread> hle_threads, guest_threads;
    std::atomic<u32> hle_finished{0};
    // Guest threads calling the HLE functions. One waiting in Lock for a ticket that never comes (a guest-style thread
    // gave up) is unwound by ending the process below.
    for (u32 i = 0; i < kHleThreads; i++) {
        hle_threads.emplace_back([&, i] {
            GuestThread g(f, 1 + i, "hle");
            const struct Finished {
                std::atomic<u32>& n;
                ~Finished() { n++; }
            } finished{hle_finished};
            start();
            try {
                for (u32 n = 0; n < rounds; n++) {
                    u64 rc;
                    while ((rc = raw(g, take, {kMutex})) == kBusy) {
                        if (!keep_waiting()) return;
                    }
                    if (rc != 0) {
                        errors++;
                        return;
                    }
                    hold();
                    if (raw(g, unlock, {kMutex}) != 0) {
                        errors++;
                        return;
                    }
                }
            } catch (const lv2::ThreadExit&) {
                stop = true;
            }
        });
    }
    // Guest code on a PPU, with lwarx/stwcx.
    guest_threads.emplace_back([&] {
        PPUContext ppu;
        ps_ppu_context_init(&ppu);
        start();
        for (u32 n = 0; n < rounds; n++) {
            if (lock) {
                u32 ticket = 0;
                for (;;) {
                    const u32 w = ps_ppu_lwarx(&ppu, b, kMutex);
                    ticket = w & 0xFFFF;
                    if (ps_ppu_stwcx(&ppu, b, kMutex, (w & 0xFFFF0000) | ((w + 1) & 0xFFFF))) break;
                }
                while ((ps_ppu_lwarx(&ppu, b, kMutex) >> 16) != ticket) {
                    if (!keep_waiting()) return;
                }
            } else {
                for (;;) {
                    const u32 w = ps_ppu_lwarx(&ppu, b, kMutex);
                    if (w >> 16 != (w & 0xFFFF)) {
                        if (!keep_waiting()) return;
                    } else if (ps_ppu_stwcx(&ppu, b, kMutex, (w & 0xFFFF0000) | ((w + 1) & 0xFFFF))) {
                        break;
                    }
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
    guest_threads.emplace_back([&] {
        constexpr u32 off = kMutex & (PS_RES_LINE_SIZE - 1);
        alignas(16) u8 line[PS_RES_LINE_SIZE];
        // Adds 1 to the u16 at `field` of the mutex and returns its old value.
        auto increment = [&](u32 field) {
            u16 old = 0;
            spu_update(b, kMutex, [&](u8* m) {
                old = ps_read16(m, field);
                ps_write16(m, field, static_cast<u16>(old + 1));
                return true;
            });
            return old;
        };
        start();
        for (u32 n = 0; n < rounds; n++) {
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
    for (std::thread& t : guest_threads) t.join();
    if (!wait_for([&] { return hle_finished.load() == kHleThreads; }, std::chrono::seconds(30))) {
        GuestThread x(f, 7, "exit");
        CHECK_THROWS_AS(lv2::sys_process_exit3(*x.t, 0), lv2::ThreadExit);
    }
    for (std::thread& t : hle_threads) t.join();

    INFO("looks at a held mutex: " << busy.load());
    CHECK(errors.load() == 0u);
    CHECK(overlaps.load() == 0u);
    REQUIRE_FALSE(stop.load()); // every thread finished its rounds (two holders lose a ticket, and everyone waits)
    CHECK(ps_read32(b, kCount) == holds);
    const u32 end = (kStart + holds) & 0xFFFF;
    CHECK(word(b) == ((end << 16) | end));
}

} // namespace

TEST_CASE("cellSync mutex: hammered by HLE TryLock and Unlock, a PPU's lwarx/stwcx. and an SPU taking tickets, it "
          "never has two holders", "[hle][sync]")
{
    hammer_mutex(false, 20000);
}

TEST_CASE("cellSync mutex: hammered by HLE Lock and Unlock, a PPU and an SPU taking tickets, it never has two holders",
    "[hle][sync]")
{
    hammer_mutex(true, 2000);
}

TEST_CASE("cellSync: a thread waiting in a blocking call unwinds when the process ends", "[hle][sync]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    constexpr u32 kBarrier = kData + 0x2010, kLfq = kData + 0x20000, kEntries = kData + 0x21000, kSrc = kData + 0x22000;
    REQUIRE(a.call("cellSyncMutexInitialize", kMutex) == 0);
    REQUIRE(a.call("cellSyncMutexLock", kMutex) == 0);
    REQUIRE(a.call("cellSyncBarrierInitialize", kBarrier, 2) == 0);
    std::memset(b + kLfq, 0, 128);
    REQUIRE(a.call("cellSyncLFQueueInitialize", kLfq, kEntries, 16, 1, 2, 0) == 0); // PPU2SPU
    REQUIRE(a.call("_cellSyncLFQueuePushBody", kLfq, kSrc, 0) == 0);                // now full

    Caller lock(f, 1, "cellSyncMutexLock", {kMutex});
    Caller barrier(f, 2, "cellSyncBarrierWait", {kBarrier});
    Caller push(f, 3, "_cellSyncLFQueuePushBody", {kLfq, kSrc, 1});
    CHECK(lock.blocked());
    CHECK(barrier.blocked());
    CHECK(push.blocked());
    CHECK_THROWS_AS(lv2::sys_process_exit3(*a.t, 0), lv2::ThreadExit);
    for (Caller* c : {&lock, &barrier, &push}) {
        c->join();
        CHECK(c->unwound);
        CHECK_FALSE(c->done());
    }
}

// ============================================================================================================== barrier

namespace {
constexpr u32 kBarrier = kData + 0x2010; // CellSyncBarrier {be s16 value, be u16 count}
u32 barrier_word(u8* b) { return ps_read32(b, kBarrier); }
} // namespace

TEST_CASE("cellSyncBarrier: RPCS3's checks; notifications up to the count, then the waits back to 0", "[hle][sync]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    CHECK(a.call("cellSyncBarrierInitialize", 0, 1) == kNullPointer);
    CHECK(a.call("cellSyncBarrierInitialize", kBarrier + 2, 1) == kAlign);
    CHECK(a.call("cellSyncBarrierInitialize", kUnmapped + 2, 0) == kAlign); // alignment before the count
    CHECK(a.call("cellSyncBarrierInitialize", kBarrier, 0) == kInval);
    CHECK(a.call("cellSyncBarrierInitialize", kBarrier, 32768) == kInval);
    CHECK(a.call("cellSyncBarrierInitialize", kUnmapped, 0) == kInval); // the count before memory
    CHECK(a.call("cellSyncBarrierInitialize", kUnmapped, 1) == kFault);
    for (const char* fn : {"cellSyncBarrierNotify", "cellSyncBarrierTryNotify", "cellSyncBarrierWait",
             "cellSyncBarrierTryWait"}) {
        INFO(fn);
        CHECK(a.call(fn, 0) == kNullPointer);
        CHECK(a.call(fn, kBarrier + 1) == kAlign);
        CHECK(a.call(fn, kUnmapped) == kFault);
    }

    ps_write32(b, kBarrier, 0x12345678);
    REQUIRE(a.call("cellSyncBarrierInitialize", kBarrier, 0x10003) == 0); // a u16 argument: 3
    CHECK(barrier_word(b) == 0x00000003);
    CHECK(a.call("cellSyncBarrierTryWait", kBarrier) == kBusy); // nobody notified yet
    REQUIRE(a.call("cellSyncBarrierTryNotify", kBarrier) == 0);
    REQUIRE(a.call("cellSyncBarrierNotify", kBarrier) == 0);
    CHECK(barrier_word(b) == 0x00020003);
    REQUIRE(a.call("cellSyncBarrierTryNotify", kBarrier) == 0); // the third: bit 15
    CHECK(barrier_word(b) == 0x80030003);
    CHECK(a.call("cellSyncBarrierTryNotify", kBarrier) == kBusy); // no more until every thread waited
    CHECK(barrier_word(b) == 0x80030003);
    REQUIRE(a.call("cellSyncBarrierTryWait", kBarrier) == 0);
    CHECK(barrier_word(b) == 0x80020003);
    REQUIRE(a.call("cellSyncBarrierWait", kBarrier) == 0);
    CHECK(barrier_word(b) == 0x80010003);
    REQUIRE(a.call("cellSyncBarrierTryWait", kBarrier) == 0); // the last: 0 again
    CHECK(barrier_word(b) == 0x00000003);
    CHECK(a.call("cellSyncBarrierTryWait", kBarrier) == kBusy);

    // A barrier of one thread.
    REQUIRE(a.call("cellSyncBarrierInitialize", kBarrier, 1) == 0);
    REQUIRE(a.call("cellSyncBarrierNotify", kBarrier) == 0);
    CHECK(barrier_word(b) == 0x80010001);
    REQUIRE(a.call("cellSyncBarrierWait", kBarrier) == 0);
    CHECK(barrier_word(b) == 0x00000001);
}

TEST_CASE("cellSyncBarrier: Wait sleeps until the last Notify, Notify while the last round's threads still wait",
    "[hle][sync]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    REQUIRE(a.call("cellSyncBarrierInitialize", kBarrier, 2) == 0);
    {
        Caller waiter(f, 1, "cellSyncBarrierWait", {kBarrier});
        CHECK(waiter.blocked());
        REQUIRE(a.call("cellSyncBarrierTryNotify", kBarrier) == 0);
        CHECK(waiter.blocked()); // one of two
        REQUIRE(a.call("cellSyncBarrierTryNotify", kBarrier) == 0);
        waiter.join();
        CHECK(waiter.result == 0);
        CHECK(barrier_word(b) == 0x80010002);
    }
    {
        // One thread of the round still has to wait: a Notify for the next round waits for it.
        Caller notifier(f, 1, "cellSyncBarrierNotify", {kBarrier});
        CHECK(notifier.blocked());
        REQUIRE(a.call("cellSyncBarrierTryWait", kBarrier) == 0);
        notifier.join();
        CHECK(notifier.result == 0);
        CHECK(barrier_word(b) == 0x00010002);
    }
}

TEST_CASE("cellSyncBarrier: four threads meet at it round after round", "[hle][sync]")
{
    constexpr u32 kThreads = 4, kRounds = 300;
    Fixture f;
    u8* b = f.base();
    {
        GuestThread a(f, 0, "init");
        REQUIRE(a.call("cellSyncBarrierInitialize", kBarrier, kThreads) == 0);
    }
    const hle::Function& notify = GuestThread::find("cellSyncBarrierNotify");
    const hle::Function& wait = GuestThread::find("cellSyncBarrierWait");
    std::vector<std::atomic<u32>> arrived(kRounds);
    std::atomic<u32> early{0}, errors{0};
    std::vector<std::thread> threads;
    for (u32 i = 0; i < kThreads; i++) {
        threads.emplace_back([&, i] {
            GuestThread g(f, 1 + i, "party");
            for (u32 r = 0; r < kRounds; r++) {
                arrived[r]++;
                if (raw(g, notify, {kBarrier}) != 0) errors++;
                if (raw(g, wait, {kBarrier}) != 0) errors++;
                if (arrived[r].load() != kThreads) early++; // through before everyone arrived
            }
        });
    }
    for (std::thread& t : threads) t.join();
    CHECK(errors == 0);
    CHECK(early == 0);
    CHECK(barrier_word(b) == kThreads);
}

// ================================================================================================================== rwm

namespace {
constexpr u32 kRwm = kData + 0x3000, kRwmBuf = kData + 0x4000, kIn = kData + 0x8000, kOut = kData + 0xC000;

void fill(u8* b, u32 addr, u32 size, u8 seed)
{
    for (u32 i = 0; i < size; i++) ps_write8(b, addr + i, static_cast<u8>(seed + i * 7));
}
} // namespace

TEST_CASE("cellSyncRwm: RPCS3's checks; the whole buffer copied; the Try forms are BUSY while it is held", "[hle][sync]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    CHECK(a.call("cellSyncRwmInitialize", 0, kRwmBuf, 128) == kNullPointer);
    CHECK(a.call("cellSyncRwmInitialize", kRwm, 0, 128) == kNullPointer);
    CHECK(a.call("cellSyncRwmInitialize", kRwm + 8, kRwmBuf, 128) == kAlign);
    CHECK(a.call("cellSyncRwmInitialize", kRwm, kRwmBuf + 64, 128) == kAlign);
    CHECK(a.call("cellSyncRwmInitialize", kRwm, kRwmBuf, 100) == kInval);
    CHECK(a.call("cellSyncRwmInitialize", kRwm, kRwmBuf, 0x4080) == kInval);
    CHECK(a.call("cellSyncRwmInitialize", kUnmapped, kRwmBuf, 128) == kFault);
    for (const char* fn : {"cellSyncRwmRead", "cellSyncRwmTryRead", "cellSyncRwmWrite", "cellSyncRwmTryWrite"}) {
        INFO(fn);
        CHECK(a.call(fn, 0, kOut) == kNullPointer);
        CHECK(a.call(fn, kRwm, 0) == kNullPointer);
        CHECK(a.call(fn, kRwm + 8, kOut) == kAlign);
        CHECK(a.call(fn, kUnmapped, kOut) == kFault);
    }

    ps_write32(b, kRwm, 0x00030001);
    REQUIRE(a.call("cellSyncRwmInitialize", kRwm, kRwmBuf, 256) == 0);
    CHECK(ps_read32(b, kRwm) == 0); // no readers, no writer
    CHECK(ps_read32(b, kRwm + 4) == 256);
    CHECK(ps_read64(b, kRwm + 8) == kRwmBuf);

    fill(b, kIn, 256, 1);
    REQUIRE(a.call("cellSyncRwmTryWrite", kRwm, kIn) == 0);
    CHECK(std::memcmp(b + kRwmBuf, b + kIn, 256) == 0);
    CHECK(ps_read32(b, kRwm) == 0);
    REQUIRE(a.call("cellSyncRwmTryRead", kRwm, kOut) == 0);
    CHECK(std::memcmp(b + kOut, b + kIn, 256) == 0);
    fill(b, kIn, 256, 2);
    REQUIRE(a.call("cellSyncRwmWrite", kRwm, kIn) == 0);
    REQUIRE(a.call("cellSyncRwmRead", kRwm, kOut) == 0);
    CHECK(std::memcmp(b + kOut, b + kIn, 256) == 0);
    CHECK(ps_read32(b, kRwm) == 0);

    // A writer holds it: neither Try form goes. A reader holds it: TryWrite doesn't, TryRead does (and leaves the
    // other reader counted).
    ps_write32(b, kRwm, 0x00000001);
    CHECK(a.call("cellSyncRwmTryRead", kRwm, kOut) == kBusy);
    CHECK(a.call("cellSyncRwmTryWrite", kRwm, kIn) == kBusy);
    CHECK(ps_read32(b, kRwm) == 0x00000001);
    ps_write32(b, kRwm, 0x00010000);
    CHECK(a.call("cellSyncRwmTryWrite", kRwm, kIn) == kBusy);
    CHECK(a.call("cellSyncRwmTryRead", kRwm, kOut) == 0);
    CHECK(ps_read32(b, kRwm) == 0x00010000);

    // Its buffer or the caller's not guest memory: FAULT, the counters left alone (Initialize doesn't look at the buffer).
    REQUIRE(a.call("cellSyncRwmInitialize", kRwm, kUnmapped, 128) == 0);
    CHECK(a.call("cellSyncRwmTryRead", kRwm, kOut) == kFault);
    CHECK(a.call("cellSyncRwmWrite", kRwm, kIn) == kFault);
    CHECK(ps_read32(b, kRwm) == 0);
    REQUIRE(a.call("cellSyncRwmInitialize", kRwm, kRwmBuf, 128) == 0);
    CHECK(a.call("cellSyncRwmRead", kRwm, kUnmapped) == kFault);
    CHECK(ps_read32(b, kRwm) == 0);
    // A size of 0 copies nothing.
    REQUIRE(a.call("cellSyncRwmInitialize", kRwm, kRwmBuf, 0) == 0);
    CHECK(a.call("cellSyncRwmTryRead", kRwm, kUnmapped) == 0);
}

TEST_CASE("cellSyncRwm: Read sleeps while a writer holds it, Write until the readers are gone", "[hle][sync]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    REQUIRE(a.call("cellSyncRwmInitialize", kRwm, kRwmBuf, 128) == 0);
    fill(b, kIn, 128, 5);

    // A reader (SPU code, say) is copying: Write takes the writer mark, then waits for the reader to finish.
    spu_update(b, kRwm, [](u8* r) {
        ps_write16(r, 0, 1);
        return true;
    });
    {
        Caller writer(f, 1, "cellSyncRwmWrite", {kRwm, kIn});
        REQUIRE(wait_for([&] { return ps_read32(b, kRwm) == 0x00010001; })); // writer marked, the reader still in
        CHECK(writer.blocked());
        spu_update(b, kRwm, [](u8* r) {
            ps_write16(r, 0, 0);
            return true;
        });
        writer.join();
        CHECK(writer.result == 0);
        CHECK(ps_read32(b, kRwm) == 0);
        CHECK(std::memcmp(b + kRwmBuf, b + kIn, 128) == 0);
    }
    // A writer holds it: Read waits for it.
    spu_update(b, kRwm, [](u8* r) {
        ps_write16(r, 2, 1);
        return true;
    });
    {
        Caller reader(f, 1, "cellSyncRwmRead", {kRwm, kOut});
        CHECK(reader.blocked());
        spu_update(b, kRwm, [](u8* r) {
            ps_write32(r, 0, 0);
            return true;
        });
        reader.join();
        CHECK(reader.result == 0);
        CHECK(ps_read32(b, kRwm) == 0);
        CHECK(std::memcmp(b + kOut, b + kIn, 128) == 0);
    }
}

TEST_CASE("cellSyncRwm: a writer and three readers on their own threads; no reader sees half a buffer", "[hle][sync]")
{
    constexpr u32 kSize = 0x1000, kReaders = 3, kWrites = 400;
    Fixture f;
    u8* b = f.base();
    {
        GuestThread a(f, 0, "init");
        REQUIRE(a.call("cellSyncRwmInitialize", kRwm, kRwmBuf, kSize) == 0);
    }
    const hle::Function& read = GuestThread::find("cellSyncRwmRead");
    const hle::Function& write = GuestThread::find("cellSyncRwmWrite");
    std::atomic<bool> done{false};
    std::atomic<u32> torn{0}, errors{0}, reads{0};
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        GuestThread g(f, 1, "writer");
        for (u32 k = 1; k <= kWrites; k++) {
            std::memset(b + kIn, static_cast<int>(k & 0xFF), kSize);
            if (raw(g, write, {kRwm, kIn}) != 0) errors++;
        }
        done = true;
    });
    for (u32 i = 0; i < kReaders; i++) {
        threads.emplace_back([&, i] {
            GuestThread g(f, 2 + i, "reader");
            const u32 out = kOut + i * kSize;
            while (!done) {
                if (raw(g, read, {kRwm, out}) != 0) errors++;
                reads++;
                for (u32 k = 1; k < kSize; k++) {
                    if (b[out + k] != b[out]) {
                        torn++;
                        break;
                    }
                }
            }
        });
    }
    for (std::thread& t : threads) t.join();
    INFO("reads: " << reads.load());
    CHECK(errors == 0);
    CHECK(torn == 0);
    CHECK(ps_read32(b, kRwm) == 0);
    CHECK(b[kRwmBuf + kSize - 1] == (kWrites & 0xFF));
}

// ================================================================================================================ queue

namespace {
constexpr u32 kQueue = kData + 0x10000, kEntries = kData + 0x11000, kSrc = kData + 0x12000, kDst = kData + 0x12100;

// A 16-byte entry holding `v` four times.
void put_entry(u8* b, u32 addr, u32 v)
{
    for (u32 i = 0; i < 16; i += 4) ps_write32(b, addr + i, v);
}

u64 queue_ctrl(u8* b) { return ps_read64(b, kQueue); }
} // namespace

TEST_CASE("cellSyncQueue: RPCS3's checks; a ring of entries in order; the Try forms are BUSY when full or empty",
    "[hle][sync]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    CHECK(a.call("cellSyncQueueInitialize", 0, kEntries, 16, 4) == kNullPointer);
    CHECK(a.call("cellSyncQueueInitialize", kQueue, 0, 16, 4) == kNullPointer);
    CHECK(a.call("cellSyncQueueInitialize", kQueue + 16, kEntries, 16, 4) == kAlign);
    CHECK(a.call("cellSyncQueueInitialize", kQueue, kEntries + 8, 16, 4) == kAlign);
    CHECK(a.call("cellSyncQueueInitialize", kQueue, kEntries, 16, 0) == kInval);
    CHECK(a.call("cellSyncQueueInitialize", kQueue, kEntries, 24, 4) == kInval);
    CHECK(a.call("cellSyncQueueInitialize", kUnmapped, kEntries, 16, 4) == kFault);
    CHECK(a.call("cellSyncQueueInitialize", kQueue, 0, 0, 4) == 0); // no buffer for entries of 0 bytes
    for (const char* fn : {"cellSyncQueuePush", "cellSyncQueueTryPush", "cellSyncQueuePop", "cellSyncQueueTryPop",
             "cellSyncQueuePeek", "cellSyncQueueTryPeek"}) {
        INFO(fn);
        CHECK(a.call(fn, 0, kSrc) == kNullPointer);
        CHECK(a.call(fn, kQueue, 0) == kNullPointer);
        CHECK(a.call(fn, kQueue + 16, kSrc) == kAlign);
        CHECK(a.call(fn, kUnmapped, kSrc) == kFault);
    }
    for (const char* fn : {"cellSyncQueueSize", "cellSyncQueueClear"}) {
        INFO(fn);
        CHECK(a.call(fn, 0) == kNullPointer);
        CHECK(a.call(fn, kQueue + 16) == kAlign);
        CHECK(a.call(fn, kUnmapped) == kFault);
    }

    ps_write64(b, kQueue, 0x0100000201000003ull);
    ps_write64(b, kQueue + 0x18, 0x1122334455667788ull);
    REQUIRE(a.call("cellSyncQueueInitialize", kQueue, kEntries, 16, 3) == 0);
    CHECK(queue_ctrl(b) == 0);
    CHECK(ps_read32(b, kQueue + 8) == 16);
    CHECK(ps_read32(b, kQueue + 12) == 3);
    CHECK(ps_read64(b, kQueue + 16) == kEntries);
    CHECK(ps_read64(b, kQueue + 0x18) == 0x1122334455667788ull); // reserved: untouched
    CHECK(a.call("cellSyncQueueSize", kQueue) == 0);
    CHECK(a.call("cellSyncQueueTryPop", kQueue, kDst) == kBusy);
    CHECK(a.call("cellSyncQueueTryPeek", kQueue, kDst) == kBusy);

    for (u32 v = 1; v <= 3; v++) {
        put_entry(b, kSrc, v);
        REQUIRE(a.call(v == 3 ? "cellSyncQueuePush" : "cellSyncQueueTryPush", kQueue, kSrc) == 0);
        CHECK(ps_read32(b, kEntries + 16 * (v - 1)) == v);
    }
    put_entry(b, kSrc, 4);
    CHECK(a.call("cellSyncQueueTryPush", kQueue, kSrc) == kBusy);
    CHECK(queue_ctrl(b) == 0x0000000000000003ull); // next 0 (wrapped), count 3
    CHECK(a.call("cellSyncQueueSize", kQueue) == 3);

    REQUIRE(a.call("cellSyncQueueTryPeek", kQueue, kDst) == 0);
    CHECK(ps_read32(b, kDst) == 1);
    REQUIRE(a.call("cellSyncQueuePeek", kQueue, kDst) == 0);
    CHECK(ps_read32(b, kDst + 12) == 1);
    CHECK(a.call("cellSyncQueueSize", kQueue) == 3);
    REQUIRE(a.call("cellSyncQueueTryPop", kQueue, kDst) == 0);
    CHECK(ps_read32(b, kDst) == 1);
    CHECK(queue_ctrl(b) == 0x0000000000000002ull);
    REQUIRE(a.call("cellSyncQueuePush", kQueue, kSrc) == 0); // 4, into the first entry
    CHECK(ps_read32(b, kEntries) == 4);
    CHECK(queue_ctrl(b) == 0x0000000100000003ull);
    for (const u32 v : {2u, 3u, 4u}) {
        REQUIRE(a.call(v == 3 ? "cellSyncQueuePop" : "cellSyncQueueTryPop", kQueue, kDst) == 0);
        CHECK(ps_read32(b, kDst) == v);
    }
    CHECK(a.call("cellSyncQueueSize", kQueue) == 0);
    CHECK(a.call("cellSyncQueueTryPop", kQueue, kDst) == kBusy);

    // Clear empties it.
    REQUIRE(a.call("cellSyncQueueTryPush", kQueue, kSrc) == 0);
    REQUIRE(a.call("cellSyncQueueTryPush", kQueue, kSrc) == 0);
    REQUIRE(a.call("cellSyncQueueClear", kQueue) == 0);
    CHECK(queue_ctrl(b) == 0);
    CHECK(a.call("cellSyncQueueTryPop", kQueue, kDst) == kBusy);

    // Entries or the caller's buffer not guest memory: FAULT, the queue as it was.
    REQUIRE(a.call("cellSyncQueueInitialize", kQueue, kUnmapped, 16, 2) == 0);
    CHECK(a.call("cellSyncQueueTryPush", kQueue, kSrc) == kFault);
    CHECK(a.call("cellSyncQueuePush", kQueue, kSrc) == kFault);
    CHECK(queue_ctrl(b) == 0);
    REQUIRE(a.call("cellSyncQueueInitialize", kQueue, kEntries, 16, 2) == 0);
    CHECK(a.call("cellSyncQueueTryPush", kQueue, kUnmapped) == kFault);
    CHECK(queue_ctrl(b) == 0);
    REQUIRE(a.call("cellSyncQueueTryPush", kQueue, kSrc) == 0);
    CHECK(a.call("cellSyncQueuePop", kQueue, kUnmapped) == kFault);
    CHECK(queue_ctrl(b) == 0x0000000100000001ull);
}

TEST_CASE("cellSyncQueue: Pop sleeps on an empty queue, Push on a full one, Clear while a pop is under way", "[hle][sync]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    REQUIRE(a.call("cellSyncQueueInitialize", kQueue, kEntries, 16, 1) == 0);
    {
        Caller pop(f, 1, "cellSyncQueuePop", {kQueue, kDst});
        CHECK(pop.blocked());
        put_entry(b, kSrc, 42);
        REQUIRE(a.call("cellSyncQueueTryPush", kQueue, kSrc) == 0);
        pop.join();
        CHECK(pop.result == 0);
        CHECK(ps_read32(b, kDst) == 42);
    }
    REQUIRE(a.call("cellSyncQueueTryPush", kQueue, kSrc) == 0); // full
    {
        put_entry(b, kSrc + 16, 43);
        Caller push(f, 1, "cellSyncQueuePush", {kQueue, kSrc + 16});
        CHECK(push.blocked());
        REQUIRE(a.call("cellSyncQueueTryPop", kQueue, kDst) == 0);
        CHECK(ps_read32(b, kDst) == 42);
        push.join();
        CHECK(push.result == 0);
        REQUIRE(a.call("cellSyncQueueTryPop", kQueue, kDst) == 0);
        CHECK(ps_read32(b, kDst) == 43);
    }
    // A pop under way (SPU code, say): Clear waits for it, then clears.
    REQUIRE(a.call("cellSyncQueueTryPush", kQueue, kSrc) == 0);
    spu_update(b, kQueue, [](u8* q) {
        ps_write8(q, 0, 1); // _pop
        return true;
    });
    {
        Caller clear(f, 1, "cellSyncQueueClear", {kQueue});
        CHECK(clear.blocked());
        spu_update(b, kQueue, [](u8* q) {
            ps_write8(q, 0, 0);
            return true;
        });
        clear.join();
        CHECK(clear.result == 0);
        CHECK(queue_ctrl(b) == 0);
    }
}

TEST_CASE("cellSyncQueue: two producers and two consumers on their own threads; every entry once, each producer's in "
          "order", "[hle][sync]")
{
    constexpr u32 kPerProducer = 3000;
    Fixture f;
    u8* b = f.base();
    {
        GuestThread a(f, 0, "init");
        REQUIRE(a.call("cellSyncQueueInitialize", kQueue, kEntries, 16, 4) == 0);
    }
    const hle::Function& push = GuestThread::find("cellSyncQueuePush");
    const hle::Function& pop = GuestThread::find("cellSyncQueuePop");
    std::vector<std::atomic<u32>> seen(2 * kPerProducer);
    std::atomic<u32> errors{0}, disorder{0};
    std::vector<std::thread> threads;
    for (u32 p = 0; p < 2; p++) {
        threads.emplace_back([&, p] {
            GuestThread g(f, 1 + p, "producer");
            const u32 src = kSrc + 0x100 * p;
            for (u32 n = 0; n < kPerProducer; n++) {
                ps_write32(b, src, p);
                ps_write32(b, src + 4, n);
                if (raw(g, push, {kQueue, src}) != 0) errors++;
            }
        });
    }
    for (u32 c = 0; c < 2; c++) {
        threads.emplace_back([&, c] {
            GuestThread g(f, 3 + c, "consumer");
            const u32 dst = kData + 0x13000 + 0x100 * c; // apart from the producers' sources
            s64 last[2] = {-1, -1};
            for (u32 n = 0; n < kPerProducer; n++) {
                if (raw(g, pop, {kQueue, dst}) != 0) errors++;
                const u32 p = ps_read32(b, dst) & 1, seq = ps_read32(b, dst + 4);
                if (static_cast<s64>(seq) <= last[p]) disorder++;
                last[p] = seq;
                if (seq < kPerProducer) seen[p * kPerProducer + seq]++;
            }
        });
    }
    for (std::thread& t : threads) t.join();
    CHECK(errors == 0);
    CHECK(disorder == 0);
    u32 once = 0;
    for (const auto& s : seen) once += s.load() == 1;
    CHECK(once == 2 * kPerProducer);
    CHECK(queue_ctrl(b) == 0);
}

// ============================================================================================================== LFQueue

namespace {
constexpr u32 kLfq = kData + 0x20000, kLfEntries = kData + 0x21000, kLfSrc = kData + 0x22000, kLfDst = kData + 0x22100,
              kOutWord = kData + 0x22200;
constexpr u32 kSpu2Ppu = 1, kPpu2Spu = 2, kAny2Any = 3;
constexpr u32 kH1 = 0x00, kH2 = 0x02, kH3 = 0x04, kH4 = 0x06, kH5 = 0x08, kH6 = 0x0A, kH7 = 0x0C, kH8 = 0x0E,
              kSize = 0x10, kDepth = 0x14, kBuffer = 0x18, kBs = 0x20, kDirection = 0x24, kV1 = 0x28, kInit = 0x2C,
              kPush2 = 0x30, kHs1 = 0x32, kPop2 = 0x50, kHs2 = 0x52, kEaSignal = 0x70, kV2 = 0x78, kEqId = 0x7C;

u16 h(u8* b, u32 off) { return ps_read16(b, kLfq + off); }

// A fresh queue: zeroed (from SDK 1.8 Initialize wants it so), then initialized.
void lf_init(GuestThread& a, u32 depth, u32 direction, u32 signal = 0)
{
    std::memset(a.f.base() + kLfq, 0, 128);
    REQUIRE(a.call("cellSyncLFQueueInitialize", kLfq, kLfEntries, 16, depth, direction, signal) == 0);
}

// SPU code's side of a queue, as a single SPU popping a PPU2SPU queue (or pushing into an SPU2PPU one) in order: the
// pointer is handed out and completed with GETLLAR/PUTLLC (in order, the completed pointer just moves on). False when
// there is no entry (no room).
bool spu_pop(u8* b, u32 depth, u32& value)
{
    s32 p = -1;
    spu_update(b, kLfq, [&](u8* q) {
        const s32 next = static_cast<s16>(ps_read16(q, kH4));
        s32 avail = ps_read16(q, kH5) - next;
        if (avail < 0) avail += 2 * static_cast<s32>(depth);
        if (avail <= 0) return false;
        p = next;
        ps_write16(q, kH4, static_cast<u16>(p + 1 >= static_cast<s32>(2 * depth) ? 0 : p + 1));
        return true;
    });
    if (p < 0) return false;
    value = ps_read32(b, kLfEntries + 16 * (static_cast<u32>(p) % depth));
    spu_update(b, kLfq, [&](u8* q) {
        ps_write16(q, kH1, static_cast<u16>(p + 1 >= static_cast<s32>(2 * depth) ? 0 : p + 1));
        return true;
    });
    return true;
}

bool spu_push(u8* b, u32 depth, u32 value)
{
    s32 p = -1;
    spu_update(b, kLfq, [&](u8* q) {
        const s32 next = static_cast<s16>(ps_read16(q, kH8));
        s32 used = next - ps_read16(q, kH1);
        if (used < 0) used += 2 * static_cast<s32>(depth);
        if (used >= static_cast<s32>(depth)) return false;
        p = next;
        ps_write16(q, kH8, static_cast<u16>(p + 1 >= static_cast<s32>(2 * depth) ? 0 : p + 1));
        return true;
    });
    if (p < 0) return false;
    put_entry(b, kLfEntries + 16 * (static_cast<u32>(p) % depth), value);
    spu_update(b, kLfq, [&](u8* q) {
        ps_write16(q, kH5, static_cast<u16>(p + 1 >= static_cast<s32>(2 * depth) ? 0 : p + 1));
        return true;
    });
    return true;
}

// A guest signal function at kSigCode (descriptor kSigOpd): stores its two arguments at kSigRecord and returns 0x55.
constexpr u32 kSigCode = kData + 0x30000, kSigOpd = kData + 0x30100, kSigRecord = kData + 0x30200;
void write_signal_function(u8* b)
{
    const u32 words[] = {
        0x3CA00000u | (kSigRecord >> 16),    // lis r5, record@h
        0x60A50000u | (kSigRecord & 0xFFFF), // ori r5, r5, record@l
        0x90650000u,                         // stw r3, 0(r5)
        0x90850004u,                         // stw r4, 4(r5)
        0x38600055u,                         // li r3, 0x55
        0x4E800020u,                         // blr
    };
    for (u32 i = 0; i < std::size(words); i++) ps_write32(b, kSigCode + 4 * i, words[i]);
    ps_write32(b, kSigOpd, kSigCode);
    ps_write32(b, kSigOpd + 4, 0);
    ps_write64(b, kSigRecord, 0);
}
} // namespace

TEST_CASE("cellSyncLFQueueInitialize: RPCS3's checks in its order; the layout; init 2 and a second Initialize, as libsre",
    "[hle][sync][lfqueue]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    std::memset(b + kLfq, 0, 128);
    const auto init = [&](u32 queue, u32 buffer, u32 size, u32 depth, u32 direction, u32 signal) {
        return a.call("cellSyncLFQueueInitialize", queue, buffer, size, depth, direction, signal);
    };
    CHECK(init(0, kLfEntries, 16, 4, kPpu2Spu, 0) == kNullPointer);
    CHECK(init(kLfq, 0, 16, 4, kPpu2Spu, 0) == kNullPointer);
    CHECK(init(kLfq, kLfEntries, 0x4010, 4, kPpu2Spu, 0) == kInval);
    CHECK(init(kLfq, kLfEntries, 24, 4, kPpu2Spu, 0) == kInval);
    CHECK(init(kLfq + 16, kLfEntries, 24, 4, kPpu2Spu, 0) == kInval); // the size before the alignment
    CHECK(init(kLfq, kLfEntries, 16, 0, kPpu2Spu, 0) == kInval);
    CHECK(init(kLfq, kLfEntries, 16, 0x8000, kPpu2Spu, 0) == kInval);
    CHECK(init(kLfq, kLfEntries, 16, 4, 4, 0) == kInval);
    CHECK(init(kLfq + 16, kLfEntries, 16, 4, kPpu2Spu, 0) == kAlign);
    CHECK(init(kLfq, kLfEntries + 8, 16, 4, kPpu2Spu, 0) == kAlign);
    CHECK(init(kUnmapped, kLfEntries, 16, 4, kPpu2Spu, 0) == kFault);
    for (u32 i = 0; i < 128; i++) CHECK(b[kLfq + i] == 0); // refused calls wrote nothing

    REQUIRE(init(kLfq, kLfEntries, 16, 0x7FFF, kPpu2Spu, 0xABCD0080) == 0);
    for (const u32 off : {kH1, kH2, kH3, kH4, kH5, kH6, kH7, kH8, kPush2, kPop2}) CHECK(h(b, off) == 0);
    CHECK(ps_read32(b, kLfq + kSize) == 16);
    CHECK(ps_read32(b, kLfq + kDepth) == 0x7FFF);
    CHECK(ps_read64(b, kLfq + kBuffer) == kLfEntries);
    CHECK(ps_read32(b, kLfq + kBs) == 0xFFFFFFFF); // no lv2 event queue attached
    CHECK(ps_read32(b, kLfq + kDirection) == kPpu2Spu);
    CHECK(ps_read32(b, kLfq + kV1) == 0);
    CHECK(ps_read32(b, kLfq + kInit) == 2); // libsre's (RPCS3 leaves 0)
    for (u32 i = 0; i < 15; i++) {
        CHECK(h(b, kHs1 + 2 * i) == 0);
        CHECK(h(b, kHs2 + 2 * i) == 0);
    }
    CHECK(ps_read64(b, kLfq + kEaSignal) == 0xABCD0080);
    CHECK(ps_read32(b, kLfq + kV2) == 0);
    CHECK(ps_read32(b, kLfq + kEqId) == 0);

    // A second Initialize with the same arguments is fine (the SPU side initializes the same queue); one that differs in
    // anything is INVAL.
    CHECK(init(kLfq, kLfEntries, 16, 0x7FFF, kPpu2Spu, 0xABCD0080) == 0);
    CHECK(init(kLfq, kLfEntries, 32, 0x7FFF, kPpu2Spu, 0xABCD0080) == kInval);
    CHECK(init(kLfq, kLfEntries, 16, 4, kPpu2Spu, 0xABCD0080) == kInval);
    CHECK(init(kLfq, kLfEntries + 16, 16, 0x7FFF, kPpu2Spu, 0xABCD0080) == kInval);
    CHECK(init(kLfq, kLfEntries, 16, 0x7FFF, kPpu2Spu, 0) == kInval);
    CHECK(init(kLfq, kLfEntries, 16, 0x7FFF, kSpu2Ppu, 0xABCD0080) == kInval);
    CHECK(ps_read32(b, kLfq + kDirection) == kPpu2Spu);

    // A queue being set up (init 1), or not zeroed: STAT.
    ps_write32(b, kLfq + kInit, 1);
    CHECK(init(kLfq, kLfEntries, 16, 4, kPpu2Spu, 0) == kStat);
    std::memset(b + kLfq, 0, 128);
    ps_write8(b, kLfq + 0x7F, 1);
    CHECK(init(kLfq, kLfEntries, 16, 4, kPpu2Spu, 0) == kStat);
    CHECK(ps_read32(b, kLfq + kInit) == 0);

    // ANY2ANY: the buffer's bit 0, m_bs's first two bytes, m_v1 and both packs all ones. Its buffer then differs from
    // the argument: a second Initialize is INVAL (as RPCS3 and libsre).
    std::memset(b + kLfq, 0, 128);
    REQUIRE(init(kLfq, kLfEntries, 16, 4, kAny2Any, 0) == 0);
    CHECK(ps_read64(b, kLfq + kBuffer) == (kLfEntries | 1));
    CHECK(ps_read32(b, kLfq + kBs) == 0xFFFF0000);
    CHECK(ps_read32(b, kLfq + kV1) == 0xFFFFFFFF);
    CHECK(h(b, kPush2) == 0xFFFF);
    CHECK(h(b, kPop2) == 0xFFFF);
    CHECK(ps_read32(b, kLfq + kInit) == 2);
    CHECK(init(kLfq, kLfEntries, 16, 4, kAny2Any, 0) == kInval);
    // No buffer for entries of 0 bytes.
    std::memset(b + kLfq, 0, 128);
    CHECK(init(kLfq, 0, 0, 4, kSpu2Ppu, 0) == 0);
}

TEST_CASE("cellSyncLFQueueInitialize: before SDK 1.8 a used queue is set up again, its event states kept",
    "[hle][sync][lfqueue]")
{
    lv2::Config config;
    config.sdk_version = 0x170000;
    Fixture f({}, config);
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    for (u32 i = 0; i < 128; i++) ps_write8(b, kLfq + i, static_cast<u8>(0x40 + i));
    ps_write32(b, kLfq + kInit, 0);
    REQUIRE(a.call("cellSyncLFQueueInitialize", kLfq, kLfEntries, 16, 4, kPpu2Spu, 0) == 0);
    CHECK(h(b, kH1) == 0);
    CHECK(h(b, kH3) == 0x4445); // kept
    CHECK(h(b, kH7) == 0x4C4D); // kept
    CHECK(h(b, kH8) == 0);
    CHECK(ps_read32(b, kLfq + kBs) == 0xFFFFFFFF);
    CHECK(ps_read32(b, kLfq + kInit) == 2);
    ps_write32(b, kLfq + kInit, 1); // being set up: set up again
    REQUIRE(a.call("cellSyncLFQueueInitialize", kLfq, kLfEntries, 16, 4, kPpu2Spu, 0) == 0);
    // init 2: before SDK 1.8 only size, depth and buffer are compared.
    CHECK(a.call("cellSyncLFQueueInitialize", kLfq, kLfEntries, 16, 4, kSpu2Ppu, 0x1000) == 0);
    CHECK(a.call("cellSyncLFQueueInitialize", kLfq, kLfEntries, 32, 4, kPpu2Spu, 0) == kInval);
}

TEST_CASE("cellSyncLFQueue: the getters, Size and Clear as RPCS3", "[hle][sync][lfqueue]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    lf_init(a, 4, kPpu2Spu, 0xABCD0080);
    const std::pair<const char*, u32> getters[] = {
        {"cellSyncLFQueueDepth", 4},
        {"cellSyncLFQueueGetDirection", kPpu2Spu},
        {"cellSyncLFQueueGetEntrySize", 16},
        {"_cellSyncLFQueueGetSignalAddress", 0xABCD0080},
        {"cellSyncLFQueueSize", 0},
    };
    for (const auto& [fn, value] : getters) {
        INFO(fn);
        CHECK(a.call(fn, 0, kOutWord) == kNullPointer);
        CHECK(a.call(fn, kLfq, 0) == kNullPointer);
        CHECK(a.call(fn, kLfq + 64, kOutWord) == kAlign);
        CHECK(a.call(fn, kUnmapped, kOutWord) == kFault);
        CHECK(a.call(fn, kLfq, kUnmapped) == kFault);
        ps_write32(b, kOutWord, 0xDEADBEEF);
        REQUIRE(a.call(fn, kLfq, kOutWord) == 0);
        CHECK(ps_read32(b, kOutWord) == value);
    }
    // Size: the pushes completed less the pops completed, modulo 2 * depth.
    ps_write16(b, kLfq + kH1, 6);
    ps_write16(b, kLfq + kH5, 1);
    REQUIRE(a.call("cellSyncLFQueueSize", kLfq, kOutWord) == 0);
    CHECK(ps_read32(b, kOutWord) == 3);
    ps_write16(b, kLfq + kH1, 1);
    ps_write16(b, kLfq + kH5, 3);
    REQUIRE(a.call("cellSyncLFQueueSize", kLfq, kOutWord) == 0);
    CHECK(ps_read32(b, kOutWord) == 2);

    // Clear: BUSY while a pointer is handed out and not completed, or SPU code waits; else the pops catch up with the
    // pushes.
    CHECK(a.call("cellSyncLFQueueClear", 0) == kNullPointer);
    CHECK(a.call("cellSyncLFQueueClear", kLfq + 64) == kAlign);
    CHECK(a.call("cellSyncLFQueueClear", kUnmapped) == kFault);
    ps_write64(b, kLfq + kH1, 0x0001000000000001ull);
    ps_write64(b, kLfq + kH5, 0x0003000000000004ull); // a push handed out (4), not completed (3)
    CHECK(a.call("cellSyncLFQueueClear", kLfq) == kBusy);
    ps_write16(b, kLfq + kH8, 3);
    ps_write16(b, kLfq + kH4, 2); // a pop handed out, not completed
    CHECK(a.call("cellSyncLFQueueClear", kLfq) == kBusy);
    ps_write16(b, kLfq + kH4, 1);
    ps_write16(b, kLfq + kPop2, 0x0400); // head 1, tail 0: a waiter
    CHECK(a.call("cellSyncLFQueueClear", kLfq) == kBusy);
    ps_write16(b, kLfq + kPop2, 0x0401); // head 1 == tail 1: nobody waits
    ps_write64(b, kLfq + kH5, 0x0003123400000003ull);
    REQUIRE(a.call("cellSyncLFQueueClear", kLfq) == 0);
    CHECK(ps_read64(b, kLfq + kH1) == 0x0003123400000003ull);
    // ANY2ANY: m_h3 and m_h7 are the waiter rings.
    lf_init(a, 4, kAny2Any);
    ps_write16(b, kLfq + kH7, 0x0400);
    CHECK(a.call("cellSyncLFQueueClear", kLfq) == kBusy);
    ps_write16(b, kLfq + kH7, 0x0401);
    CHECK(a.call("cellSyncLFQueueClear", kLfq) == 0);
    // An s16 against a u16, as RPCS3 and libsre: pointers from 0x8000 never compare equal.
    lf_init(a, 0x7FFF, kPpu2Spu);
    ps_write16(b, kLfq + kH5, 0x8000);
    ps_write16(b, kLfq + kH8, 0x8000);
    CHECK(a.call("cellSyncLFQueueClear", kLfq) == kBusy);
}

TEST_CASE("_cellSyncLFQueuePushBody and PopBody: entries in order, AGAIN when full or empty, PERM the other way, NOSYS "
          "for ANY2ANY", "[hle][sync][lfqueue]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    for (const char* fn : {"_cellSyncLFQueuePushBody", "_cellSyncLFQueuePopBody"}) {
        INFO(fn);
        CHECK(a.call(fn, 0, kLfSrc, 0) == kNullPointer);
        CHECK(a.call(fn, kLfq, 0, 0) == kNullPointer);
        CHECK(a.call(fn, kLfq + 64, kLfSrc, 0) == kAlign);
        CHECK(a.call(fn, kLfq, kLfSrc + 8, 0) == kAlign);
        CHECK(a.call(fn, kUnmapped, kLfSrc, 0) == kFault);
    }

    // The PPU pushes into a PPU2SPU queue; SPU code pops.
    lf_init(a, 4, kPpu2Spu);
    for (u32 v = 1; v <= 4; v++) {
        put_entry(b, kLfSrc, v);
        REQUIRE(a.call("_cellSyncLFQueuePushBody", kLfq, kLfSrc, v % 2) == 0);
        CHECK(ps_read32(b, kLfEntries + 16 * (v - 1)) == v);
        CHECK(h(b, kH8) == v);
        CHECK(h(b, kH5) == v);
    }
    put_entry(b, kLfSrc, 5);
    CHECK(a.call("_cellSyncLFQueuePushBody", kLfq, kLfSrc, 0) == kAgain);
    CHECK(h(b, kH8) == 4);
    CHECK(a.call("_cellSyncLFQueuePopBody", kLfq, kLfDst, 0) == kPerm);
    // Pointers run modulo 2 * depth: pop and push around the ring twice.
    for (u32 v = 5; v <= 20; v++) {
        u32 value = 0;
        REQUIRE(spu_pop(b, 4, value));
        CHECK(value == v - 4);
        put_entry(b, kLfSrc, v);
        REQUIRE(a.call("_cellSyncLFQueuePushBody", kLfq, kLfSrc, 0) == 0);
        CHECK(h(b, kH8) == v % 8);
        CHECK(h(b, kH5) == v % 8);
    }
    REQUIRE(a.call("cellSyncLFQueueSize", kLfq, kOutWord) == 0);
    CHECK(ps_read32(b, kOutWord) == 4);
    // The caller's buffer, or the entry, not guest memory: FAULT, the pointers as they were.
    {
        u32 value = 0;
        REQUIRE(spu_pop(b, 4, value));
    }
    CHECK(a.call("_cellSyncLFQueuePushBody", kLfq, kUnmapped, 0) == kFault);
    CHECK(h(b, kH8) == 4);
    lf_init(a, 4, kPpu2Spu);
    ps_write64(b, kLfq + kBuffer, kUnmapped);
    CHECK(a.call("_cellSyncLFQueuePushBody", kLfq, kLfSrc, 1) == kFault);
    CHECK(h(b, kH8) == 0);

    // The PPU pops from an SPU2PPU queue; SPU code pushes.
    lf_init(a, 4, kSpu2Ppu);
    CHECK(a.call("_cellSyncLFQueuePopBody", kLfq, kLfDst, 0) == kAgain);
    CHECK(a.call("_cellSyncLFQueuePushBody", kLfq, kLfSrc, 0) == kPerm);
    for (u32 v = 10; v < 13; v++) REQUIRE(spu_push(b, 4, v));
    for (u32 v = 10; v < 13; v++) {
        REQUIRE(a.call("_cellSyncLFQueuePopBody", kLfq, kLfDst, 1) == 0);
        CHECK(ps_read32(b, kLfDst) == v);
    }
    CHECK(h(b, kH4) == 3);
    CHECK(h(b, kH1) == 3);
    CHECK(a.call("_cellSyncLFQueuePopBody", kLfq, kLfDst, 0) == kAgain);

    // ANY2ANY: RPCS3's *2 functions are todo stubs: NOSYS.
    lf_init(a, 4, kAny2Any);
    CHECK(a.call("_cellSyncLFQueuePushBody", kLfq, kLfSrc, 1) == kNosys);
    CHECK(a.call("_cellSyncLFQueuePopBody", kLfq, kLfDst, 1) == kNosys);
}

TEST_CASE("_cellSyncLFQueueGet/CompletePush/PopPointer: pointers completed out of order, as RPCS3's bitmap", "[hle][sync][lfqueue]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    lf_init(a, 4, kPpu2Spu);
    // RPCS3 checks nothing; a queue that is not guest memory is FAULT, a misaligned one ALIGN, as is the pointer.
    CHECK(a.call("_cellSyncLFQueueGetPushPointer", kUnmapped, kOutWord, 0, 0) == kFault);
    CHECK(a.call("_cellSyncLFQueueGetPushPointer", kLfq + 64, kOutWord, 0, 0) == kAlign);
    CHECK(a.call("_cellSyncLFQueueGetPushPointer", kLfq, kUnmapped, 0, 0) == kFault);
    CHECK(a.call("_cellSyncLFQueueCompletePushPointer", 0, 0, 0) == kFault);
    CHECK(a.call("_cellSyncLFQueueGetPopPointer", kLfq, kOutWord, 0, 0, 0) == kPerm);
    CHECK(a.call("_cellSyncLFQueueCompletePopPointer", kLfq, 0, 0, 0) == kPerm);

    for (u32 p = 0; p < 3; p++) {
        REQUIRE(a.call("_cellSyncLFQueueGetPushPointer", kLfq, kOutWord, 0, 0) == 0);
        CHECK(ps_read32(b, kOutWord) == p);
    }
    CHECK(h(b, kH8) == 3);
    // 2 first: nothing completed in a row yet, its bit (15 - 2) kept.
    REQUIRE(a.call("_cellSyncLFQueueCompletePushPointer", kLfq, 2, 0) == 0);
    CHECK(h(b, kH5) == 0);
    CHECK(h(b, kH6) == 0x2000);
    // 0: completed moves to 1; the bitmap moves with it.
    REQUIRE(a.call("_cellSyncLFQueueCompletePushPointer", kLfq, 0, 0) == 0);
    CHECK(h(b, kH5) == 1);
    CHECK(h(b, kH6) == 0x4000);
    // 1: past 2 as well.
    REQUIRE(a.call("_cellSyncLFQueueCompletePushPointer", kLfq, 1, 0) == 0);
    CHECK(h(b, kH5) == 3);
    CHECK(h(b, kH6) == 0);
    REQUIRE(a.call("_cellSyncLFQueueGetPushPointer", kLfq, kOutWord, 1, 0) == 0);
    CHECK(ps_read32(b, kOutWord) == 3);
    CHECK(a.call("_cellSyncLFQueueGetPushPointer", kLfq, kOutWord, 0, 0) == kAgain); // 4 handed out: full
    REQUIRE(a.call("_cellSyncLFQueueCompletePushPointer", kLfq, 3, 0) == 0);
    CHECK(h(b, kH5) == 4);

    // The pop side of an SPU2PPU queue, the same way.
    lf_init(a, 4, kSpu2Ppu);
    CHECK(a.call("_cellSyncLFQueueGetPopPointer", kLfq, kOutWord, 0, 0, 0) == kAgain);
    for (u32 v = 0; v < 3; v++) REQUIRE(spu_push(b, 4, v));
    for (u32 p = 0; p < 3; p++) {
        REQUIRE(a.call("_cellSyncLFQueueGetPopPointer", kLfq, kOutWord, 0, 0x1234, 0) == 0); // arg4 unused
        CHECK(ps_read32(b, kOutWord) == p);
    }
    CHECK(h(b, kH4) == 3);
    REQUIRE(a.call("_cellSyncLFQueueCompletePopPointer", kLfq, 2, 0, 0) == 0);
    CHECK(h(b, kH1) == 0);
    CHECK(h(b, kH2) == 0x2000);
    REQUIRE(a.call("_cellSyncLFQueueCompletePopPointer", kLfq, 0, 0, 0) == 0);
    CHECK(h(b, kH1) == 1);
    CHECK(h(b, kH2) == 0x4000);
    REQUIRE(a.call("_cellSyncLFQueueCompletePopPointer", kLfq, 1, 0, 0) == 0);
    CHECK(h(b, kH1) == 3);
    CHECK(h(b, kH2) == 0);
    CHECK(a.call("_cellSyncLFQueueGetPushPointer", kLfq, kOutWord, 0, 0) == kPerm);
}

TEST_CASE("_cellSyncLFQueueComplete*Pointer: an SPU waiter is taken from the ring and signalled through the guest's "
          "function; noQueueFull waits for the ring instead", "[hle][sync][lfqueue]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    write_signal_function(b);

    // Push side: SPU code (ID 7) waits to pop, in the push ring (head 0, tail 1).
    lf_init(a, 4, kPpu2Spu, 0x12345600);
    REQUIRE(a.call("_cellSyncLFQueueGetPushPointer", kLfq, kOutWord, 0, 0) == 0);
    ps_write16(b, kLfq + kHs1, 7);
    ps_write16(b, kLfq + kPush2, 0x0001);
    CHECK(a.call("_cellSyncLFQueueCompletePushPointer", kLfq, 0, kSigOpd) == 0x55); // the function's result
    CHECK(ps_read32(b, kSigRecord) == 0x12345600);                                    // eaSignal
    CHECK(ps_read32(b, kSigRecord + 4) == 7);                                         // the waiter
    CHECK(h(b, kPush2) == 0x0401); // the head moved on
    CHECK(h(b, kH5) == 1);
    // Nobody left waiting: no signal.
    ps_write64(b, kSigRecord, 0);
    REQUIRE(a.call("_cellSyncLFQueueGetPushPointer", kLfq, kOutWord, 0, 0) == 0);
    REQUIRE(a.call("_cellSyncLFQueueCompletePushPointer", kLfq, 1, kSigOpd) == 0);
    CHECK(ps_read64(b, kSigRecord) == 0);
    CHECK(h(b, kH5) == 2);
    // A signal function that is not guest memory: FAULT, the queue as it was.
    ps_write16(b, kLfq + kHs1 + 2, 8);
    ps_write16(b, kLfq + kPush2, 0x0402);
    REQUIRE(a.call("_cellSyncLFQueueGetPushPointer", kLfq, kOutWord, 0, 0) == 0);
    CHECK(a.call("_cellSyncLFQueueCompletePushPointer", kLfq, 2, kUnmapped) == kFault);
    CHECK(h(b, kPush2) == 0x0402);
    CHECK(h(b, kH5) == 2);
    CHECK(a.call("_cellSyncLFQueueCompletePushPointer", kLfq, 2, kSigOpd) == 0x55);
    CHECK(ps_read32(b, kSigRecord + 4) == 8);
    CHECK(h(b, kPush2) == 0x0802);

    // Pop side: SPU code (ID 9) waits to push, in the pop ring.
    lf_init(a, 4, kSpu2Ppu, 0x0BAD0000);
    REQUIRE(spu_push(b, 4, 1));
    REQUIRE(spu_push(b, 4, 2));
    REQUIRE(a.call("_cellSyncLFQueueGetPopPointer", kLfq, kOutWord, 0, 0, 0) == 0);
    ps_write16(b, kLfq + kHs2, 9);
    ps_write16(b, kLfq + kPop2, 0x0001);
    CHECK(a.call("_cellSyncLFQueueCompletePopPointer", kLfq, 0, kSigOpd, 0) == 0x55);
    CHECK(ps_read32(b, kSigRecord) == 0x0BAD0000);
    CHECK(ps_read32(b, kSigRecord + 4) == 9);
    CHECK(h(b, kPop2) == 0x0401);
    CHECK(h(b, kH1) == 1);
    // noQueueFull: no signal, and the completion waits until nobody waits (SPU code takes its waiter out).
    ps_write64(b, kSigRecord, 0);
    REQUIRE(a.call("_cellSyncLFQueueGetPopPointer", kLfq, kOutWord, 0, 0, 0) == 0);
    ps_write16(b, kLfq + kHs2 + 2, 11);
    ps_write16(b, kLfq + kPop2, 0x0402);
    {
        Caller complete(f, 1, "_cellSyncLFQueueCompletePopPointer", {kLfq, 1, kSigOpd, 1});
        CHECK(complete.blocked());
        CHECK(h(b, kH1) == 1);
        spu_update(b, kLfq, [](u8* q) {
            ps_write16(q, kPop2, 0x0802); // head 2 == tail 2
            return true;
        });
        complete.join();
        CHECK(complete.result == 0);
    }
    CHECK(ps_read64(b, kSigRecord) == 0);
    CHECK(h(b, kH1) == 2);
}

TEST_CASE("_cellSyncLFQueueGetPushPointer with the event queue: STAT without one, BUSY while a push is pending, a "
          "receive while full", "[hle][sync][lfqueue]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    constexpr u32 kEqOut = kData + 0x23000, kAttr = kData + 0x23010;
    ps_write32(b, kAttr, lv2::SYS_SYNC_FIFO);
    ps_write32(b, kAttr + 4, lv2::SYS_PPU_QUEUE);
    std::memcpy(b + kAttr + 8, "lfqueue", 8);
    REQUIRE(lv2::sys_event_queue_create(*a.t, kEqOut, kAttr, 0, 4) == 0);
    const u32 eq = ps_read32(b, kEqOut);

    lf_init(a, 2, kPpu2Spu);
    for (u32 v = 0; v < 2; v++) REQUIRE(a.call("_cellSyncLFQueuePushBody", kLfq, kLfSrc, 0) == 0); // full
    CHECK(a.call("_cellSyncLFQueueGetPushPointer", kLfq, kOutWord, 1, 1) == kStat); // m_bs: none attached
    ps_write32(b, kLfq + kBs, 0);
    ps_write32(b, kLfq + kEqId, eq);
    CHECK(a.call("_cellSyncLFQueueGetPushPointer", kLfq, kOutWord, 0, 1) == kAgain);
    ps_write16(b, kLfq + kH8, 3); // a push handed out, not completed
    CHECK(a.call("_cellSyncLFQueueGetPushPointer", kLfq, kOutWord, 1, 1) == kBusy);
    ps_write16(b, kLfq + kH8, 2);

    // Full: the call waits on the lv2 queue (isBlocking 1 stores nothing), and goes on after SPU code popped and sent an
    // event.
    {
        Caller get(f, 1, "_cellSyncLFQueueGetPushPointer", {kLfq, kOutWord, 1, 1});
        REQUIRE(get.parked_on(lv2::WaitKind::EventQueue));
        CHECK(h(b, kH7) == 0);
        u32 value = 0;
        REQUIRE(spu_pop(b, 2, value));
        CHECK(get.blocked()); // no event yet
        REQUIRE(f.kernel->event_queue(eq)->send(lv2::Event{}) == 0);
        get.join();
        CHECK(get.result == 0);
        CHECK(ps_read32(b, kOutWord) == 2);
        CHECK(h(b, kH8) == 3);
        CHECK(h(b, kH7) == 0);
    }
    REQUIRE(a.call("_cellSyncLFQueueCompletePushPointer", kLfq, 2, 0) == 0);
    // isBlocking 3 marks the push side (m_h7 = 3) while it waits; the next pass clears it.
    {
        Caller get(f, 1, "_cellSyncLFQueueGetPushPointer", {kLfq, kOutWord, 3, 1});
        REQUIRE(get.parked_on(lv2::WaitKind::EventQueue));
        CHECK(h(b, kH7) == 3);
        u32 value = 0;
        REQUIRE(spu_pop(b, 2, value));
        REQUIRE(f.kernel->event_queue(eq)->send(lv2::Event{}) == 0);
        get.join();
        CHECK(get.result == 0);
        CHECK(ps_read32(b, kOutWord) == 3);
        CHECK(h(b, kH8) == 0); // 3 + 1 wraps at 2 * depth
        CHECK(h(b, kH7) == 0);
    }
}

TEST_CASE("cellSyncLFQueue: a PPU pushing with PushBody against an SPU popping with GETLLAR/PUTLLC, and the other way",
    "[hle][sync][lfqueue]")
{
    constexpr u32 kItems = 3001, kRing = 4;
    Fixture f;
    u8* b = f.base();
    for (const bool ppu_pushes : {true, false}) {
        INFO(ppu_pushes);
        {
            GuestThread a(f, 0, "init");
            lf_init(a, kRing, ppu_pushes ? kPpu2Spu : kSpu2Ppu);
        }
        const hle::Function& body = GuestThread::find(ppu_pushes ? "_cellSyncLFQueuePushBody" : "_cellSyncLFQueuePopBody");
        std::atomic<u32> errors{0}, disorder{0};
        std::atomic<bool> stop{false};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::thread ppu([&] {
            GuestThread g(f, 1, "ppu");
            for (u32 n = 0; n < kItems; n++) {
                if (ppu_pushes) put_entry(b, kLfSrc, n);
                if (raw(g, body, {kLfq, ppu_pushes ? kLfSrc : kLfDst, 1}) != 0) errors++;
                if (!ppu_pushes && ps_read32(b, kLfDst) != n) disorder++;
            }
        });
        std::thread spu([&] {
            for (u32 n = 0; n < kItems && !stop; n++) {
                u32 value = 0;
                while (!(ppu_pushes ? spu_pop(b, kRing, value) : spu_push(b, kRing, n))) {
                    if (std::chrono::steady_clock::now() > deadline) {
                        stop = true;
                        return;
                    }
                    std::this_thread::yield();
                }
                if (ppu_pushes && value != n) disorder++;
            }
        });
        spu.join();
        if (stop) { // the PPU side may wait for ever: end the process to unwind it
            GuestThread x(f, 2, "exit");
            CHECK_THROWS_AS(lv2::sys_process_exit3(*x.t, 0), lv2::ThreadExit);
        }
        ppu.join();
        REQUIRE_FALSE(stop);
        CHECK(errors == 0);
        CHECK(disorder == 0);
        const u16 end = kItems % (2 * kRing);
        for (const u32 off : {kH1, kH4, kH5, kH8}) CHECK(h(b, off) == end);
        CHECK(h(b, kH2) == 0);
        CHECK(h(b, kH6) == 0);
    }
}
