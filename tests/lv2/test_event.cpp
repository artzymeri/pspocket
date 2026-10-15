// sys_event_queue / sys_event_port and sys_event_flag through the dispatcher.

#include "lv2_test_util.hpp"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

namespace {

u32 make_queue(Fixture& f, Caller& c, s32 size, u32 protocol = SYS_SYNC_FIFO, u32 type = SYS_PPU_QUEUE, u64 key = 0)
{
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kQueueCreate, out, queue_attr(f, protocol, type), key, size) == CELL_OK);
    return f.r32(out);
}

u32 make_port(Fixture& f, Caller& c, s32 type = SYS_EVENT_PORT_LOCAL, u64 name = 0)
{
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kPortCreate, out, type, name) == CELL_OK);
    return f.r32(out);
}

u32 make_flag(Fixture& f, Caller& c, u64 init, u32 protocol = SYS_SYNC_FIFO, u32 type = SYS_SYNC_WAITER_MULTIPLE)
{
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kFlagCreate, out, sync_attr(f, protocol, type), init) == CELL_OK);
    return f.r32(out);
}

} // namespace

TEST_CASE("lv2 event queue: creation, ports, FIFO delivery, tryreceive and drain", "[lv2][event]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 out = f.alloc(4);
    CHECK(c.call(kQueueCreate, out, queue_attr(f, SYS_SYNC_FIFO), 0, 0) == rc(CELL_EINVAL));
    CHECK(c.call(kQueueCreate, out, queue_attr(f, SYS_SYNC_FIFO), 0, 128) == rc(CELL_EINVAL));
    CHECK(c.call(kQueueCreate, out, queue_attr(f, 7), 0, 16) == rc(CELL_EINVAL));
    CHECK(c.call(kQueueCreate, out, queue_attr(f, SYS_SYNC_FIFO, 3), 0, 16) == rc(CELL_EINVAL));
    CHECK(c.call(kPortCreate, out, 2, 0) == rc(CELL_EINVAL));

    const u32 q = make_queue(f, c, 4);
    const u32 p = make_port(f, c);
    CHECK(c.call(kPortSend, p, 1, 2, 3) == rc(CELL_ENOTCONN));
    CHECK(c.call(kPortConnectLocal, p, 0x8d00ff00) == rc(CELL_ESRCH));
    REQUIRE(c.call(kPortConnectLocal, p, q) == CELL_OK);
    CHECK(c.call(kPortConnectLocal, p, q) == rc(CELL_EISCONN));

    for (u64 i = 0; i < 4; i++) CHECK(c.call(kPortSend, p, i, i * 10, i * 100) == CELL_OK);
    CHECK(c.call(kPortSend, p, 9, 9, 9) == rc(CELL_EBUSY)); // full: not an error for the guest

    // The event comes back in r4..r7, as lv2 returns it.
    REQUIRE(c.call(kQueueReceive, q, 0, 0) == CELL_OK);
    CHECK(c.ctx.gpr[4] == ((1ull << 32) | p)); // source: pid 1, port ID (unnamed port)
    CHECK(c.ctx.gpr[5] == 0);
    CHECK(c.ctx.gpr[6] == 0);
    CHECK(c.ctx.gpr[7] == 0);

    const u32 events = f.alloc(32 * 8);
    const u32 number = f.alloc(4);
    REQUIRE(c.call(kQueueTryReceive, q, events, 8, number) == CELL_OK);
    REQUIRE(f.r32(number) == 3);
    for (u32 i = 0; i < 3; i++) {
        CHECK(f.r64(events + i * 32) == ((1ull << 32) | p));
        CHECK(f.r64(events + i * 32 + 8) == i + 1);
        CHECK(f.r64(events + i * 32 + 16) == (i + 1) * 10);
        CHECK(f.r64(events + i * 32 + 24) == (i + 1) * 100);
    }
    REQUIRE(c.call(kQueueTryReceive, q, events, 8, number) == CELL_OK);
    CHECK(f.r32(number) == 0);

    CHECK(c.call(kPortSend, p, 1, 1, 1) == CELL_OK);
    CHECK(c.call(kPortSend, p, 2, 2, 2) == CELL_OK);
    CHECK(c.call(kQueueDrain, q) == CELL_OK);
    REQUIRE(c.call(kQueueTryReceive, q, events, 8, number) == CELL_OK);
    CHECK(f.r32(number) == 0);

    // A named port sends its name as the source.
    const u32 named = make_port(f, c, SYS_EVENT_PORT_LOCAL, 0xABCDull);
    REQUIRE(c.call(kPortConnectLocal, named, q) == CELL_OK);
    REQUIRE(c.call(kPortSend, named, 5, 6, 7) == CELL_OK);
    REQUIRE(c.call(kQueueReceive, q, 0, 0) == CELL_OK);
    CHECK(c.ctx.gpr[4] == 0xABCD);
    CHECK(c.ctx.gpr[5] == 5);

    CHECK(c.call(kPortDestroy, p) == rc(CELL_EISCONN));
    CHECK(c.call(kPortDisconnect, p) == CELL_OK);
    CHECK(c.call(kPortDisconnect, p) == rc(CELL_ENOTCONN));
    CHECK(c.call(kPortSend, p, 1, 1, 1) == rc(CELL_ENOTCONN));
    CHECK(c.call(kPortDestroy, p) == CELL_OK);
    CHECK(c.call(kPortSend, p, 1, 1, 1) == rc(CELL_ESRCH));
}

TEST_CASE("lv2 event queue: blocked receivers, timeouts, forced destroy, SPU queues", "[lv2][event]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 q = make_queue(f, c, 16);
    const u32 p = make_port(f, c);
    REQUIRE(c.call(kPortConnectLocal, p, q) == CELL_OK);

    const auto t0 = std::chrono::steady_clock::now();
    CHECK(c.call(kQueueReceive, q, 0, 15000) == rc(CELL_ETIMEDOUT));
    CHECK(elapsed_ms(t0) >= 14);

    // A blocked receiver gets the event directly.
    std::thread receiver([&] {
        Caller me(*f.kernel, "receiver");
        THREAD_REQUIRE(me.call(kQueueReceive, q, 0, 0) == CELL_OK);
        THREAD_CHECK(me.ctx.gpr[5] == 0x1111);
        THREAD_CHECK(me.ctx.gpr[7] == 0x3333);
    });
    REQUIRE(eventually([&] { return queued<EventQueue>(*f.kernel, q) == 1; }));
    REQUIRE(c.call(kPortSend, p, 0x1111, 0x2222, 0x3333) == CELL_OK);
    receiver.join();

    // Destroy: EBUSY with a waiter unless forced; forced waiters get ECANCELED.
    std::thread canceled([&] {
        Caller me(*f.kernel, "receiver");
        THREAD_CHECK(me.call(kQueueReceive, q, 0, 0) == rc(CELL_ECANCELED));
    });
    REQUIRE(eventually([&] { return queued<EventQueue>(*f.kernel, q) == 1; }));
    CHECK(c.call(kQueueDestroy, q, 2) == rc(CELL_EINVAL));
    CHECK(c.call(kQueueDestroy, q, 0) == rc(CELL_EBUSY));
    CHECK(c.call(kQueueDestroy, q, SYS_EVENT_QUEUE_DESTROY_FORCE) == CELL_OK);
    canceled.join();
    CHECK(c.call(kPortSend, p, 1, 1, 1) == rc(CELL_ENOTCONN)); // the port's queue is gone
    CHECK(c.call(kPortDestroy, p) == CELL_OK);                  // not connected any more
    CHECK(c.call(kQueueReceive, q, 0, 0) == rc(CELL_ESRCH));

    // SPU queues aren't received by PPU threads.
    const u32 spu = make_queue(f, c, 8, SYS_SYNC_PRIORITY, SYS_SPU_QUEUE);
    const u32 number = f.alloc(4);
    CHECK(c.call(kQueueReceive, spu, 0, 0) == rc(CELL_EINVAL));
    CHECK(c.call(kQueueTryReceive, spu, f.alloc(32), 1, number) == rc(CELL_EINVAL));
}

TEST_CASE("lv2 event queue: IPC keys and IPC ports", "[lv2][event]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u64 key = 0x8000000012345678ull;
    const u32 q = make_queue(f, c, 8, SYS_SYNC_FIFO, SYS_PPU_QUEUE, key);
    const u32 out = f.alloc(4);
    CHECK(c.call(kQueueCreate, out, queue_attr(f, SYS_SYNC_FIFO), key, 8) == rc(CELL_EEXIST));
    const u32 ipc = make_port(f, c, SYS_EVENT_PORT_IPC);
    const u32 local = make_port(f, c, SYS_EVENT_PORT_LOCAL);
    CHECK(c.call(kPortConnectIpc, ipc, 0) == rc(CELL_EINVAL));
    CHECK(c.call(kPortConnectIpc, ipc, key + 1) == rc(CELL_ESRCH));
    CHECK(c.call(kPortConnectIpc, local, key) == rc(CELL_EINVAL));
    CHECK(c.call(kPortConnectLocal, ipc, q) == rc(CELL_EINVAL));
    REQUIRE(c.call(kPortConnectIpc, ipc, key) == CELL_OK);
    REQUIRE(c.call(kPortSend, ipc, 42, 0, 0) == CELL_OK);
    REQUIRE(c.call(kQueueReceive, q, 0, 0) == CELL_OK);
    CHECK(c.ctx.gpr[5] == 42);
    REQUIRE(c.call(kPortDisconnect, ipc) == CELL_OK);
    REQUIRE(c.call(kQueueDestroy, q, 0) == CELL_OK);
    CHECK(c.call(kPortConnectIpc, ipc, key) == rc(CELL_ESRCH)); // the key went with the queue
}

TEST_CASE("lv2 event queue: per-sender order holds under concurrent senders", "[lv2][event][stress]")
{
    Fixture f;
    Caller c(*f.kernel, "receiver");
    const u32 q = make_queue(f, c, 127, SYS_SYNC_PRIORITY);
    constexpr int kSenders = 4, kEvents = 3000;
    std::vector<u32> ports;
    for (int i = 0; i < kSenders; i++) {
        ports.push_back(make_port(f, c, SYS_EVENT_PORT_LOCAL, 0x100 + i));
        REQUIRE(c.call(kPortConnectLocal, ports.back(), q) == CELL_OK);
    }
    std::vector<std::thread> pool;
    for (int i = 0; i < kSenders; i++) {
        pool.emplace_back([&, i] {
            Caller me(*f.kernel, "sender");
            for (u64 n = 0; n < kEvents;) {
                const s32 r = me.call(kPortSend, ports[i], n, i, 0);
                if (r == CELL_OK) {
                    n++;
                } else {
                    THREAD_REQUIRE(r == rc(CELL_EBUSY));
                    std::this_thread::yield();
                }
            }
        });
    }
    std::vector<u64> next(kSenders, 0);
    bool in_order = true;
    for (int got = 0; got < kSenders * kEvents; got++) {
        REQUIRE(c.call(kQueueReceive, q, 0, 0) == CELL_OK);
        const u64 sender = c.ctx.gpr[4] - 0x100;
        REQUIRE(sender < kSenders);
        if (c.ctx.gpr[5] != next[sender]) in_order = false;
        next[sender] = c.ctx.gpr[5] + 1;
    }
    for (auto& t : pool) t.join();
    CHECK(in_order);
    for (int i = 0; i < kSenders; i++) CHECK(next[i] == kEvents);
}

TEST_CASE("lv2 event flag: modes, clear modes, trywait and the result pointer", "[lv2][event_flag]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 out = f.alloc(4);
    CHECK(c.call(kFlagCreate, out, sync_attr(f, 7, SYS_SYNC_WAITER_MULTIPLE), 0) == rc(CELL_EINVAL));
    CHECK(c.call(kFlagCreate, out, sync_attr(f, SYS_SYNC_FIFO, 0), 0) == rc(CELL_EINVAL));

    const u32 e = make_flag(f, c, 0b1010);
    const u32 res = f.alloc(8);
    CHECK(c.call(kFlagTryWait, e, 0b0010, SYS_EVENT_FLAG_WAIT_AND, res) == CELL_OK);
    CHECK(f.r64(res) == 0b1010);
    f.w64(res, 0xFFFF);
    CHECK(c.call(kFlagTryWait, e, 0b0110, SYS_EVENT_FLAG_WAIT_AND, res) == rc(CELL_EBUSY));
    CHECK(f.r64(res) == 0); // always written
    CHECK(c.call(kFlagTryWait, e, 0b0110, SYS_EVENT_FLAG_WAIT_OR, res) == CELL_OK);
    CHECK(c.call(kFlagTryWait, e, 0b0010, SYS_EVENT_FLAG_WAIT_AND | SYS_EVENT_FLAG_WAIT_CLEAR, res) == CELL_OK);
    CHECK(f.r64(res) == 0b1010); // the pattern before clearing
    const u32 pat = f.alloc(8);
    REQUIRE(c.call(kFlagGet, e, pat) == CELL_OK);
    CHECK(f.r64(pat) == 0b1000);
    CHECK(c.call(kFlagWait, e, 0b1000, SYS_EVENT_FLAG_WAIT_OR | SYS_EVENT_FLAG_WAIT_CLEAR_ALL, res, 0) == CELL_OK);
    REQUIRE(c.call(kFlagGet, e, pat) == CELL_OK);
    CHECK(f.r64(pat) == 0);

    CHECK(c.call(kFlagTryWait, e, 1, 3, res) == rc(CELL_EINVAL));
    CHECK(c.call(kFlagTryWait, e, 1, SYS_EVENT_FLAG_WAIT_AND | 0x30, res) == rc(CELL_EINVAL));
    CHECK(c.call(kFlagWait, e, 1, 0, res, 0) == rc(CELL_EINVAL));

    REQUIRE(c.call(kFlagSet, e, 0b11) == CELL_OK);
    REQUIRE(c.call(kFlagClear, e, 0b01) == CELL_OK); // pattern &= bitptn
    REQUIRE(c.call(kFlagGet, e, pat) == CELL_OK);
    CHECK(f.r64(pat) == 0b01);
    CHECK(c.call(kFlagGet, e, 0) == rc(CELL_EFAULT));
    f.w64(pat, 0x55);
    CHECK(c.call(kFlagGet, 0x98001234, pat) == rc(CELL_ESRCH));
    CHECK(f.r64(pat) == 0);
    CHECK(c.call(kFlagDestroy, e) == CELL_OK);
    CHECK(c.call(kFlagSet, e, 1) == rc(CELL_ESRCH));
}

TEST_CASE("lv2 event flag: set satisfies waiters in protocol order, applying each clear", "[lv2][event_flag]")
{
    const auto protocol = GENERATE(SYS_SYNC_FIFO, SYS_SYNC_PRIORITY);
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 e = make_flag(f, c, 0, protocol);
    struct W {
        s32 prio;
        u64 bits;
        u32 mode;
    };
    const W waiters[] = {
        {2000, 0b01, SYS_EVENT_FLAG_WAIT_AND | SYS_EVENT_FLAG_WAIT_CLEAR}, // A
        {100, 0b01, SYS_EVENT_FLAG_WAIT_AND | SYS_EVENT_FLAG_WAIT_CLEAR},  // B: same bit, higher priority
        {1500, 0b10, SYS_EVENT_FLAG_WAIT_OR},                              // C
    };
    std::atomic<int> done[3] = {0, 0, 0};
    u64 results[3] = {};
    std::vector<std::thread> pool;
    for (int i = 0; i < 3; i++) {
        pool.emplace_back([&, i] {
            Caller me(*f.kernel, "waiter", waiters[i].prio);
            const u32 res = f.alloc(8);
            THREAD_REQUIRE(me.call(kFlagWait, e, waiters[i].bits, waiters[i].mode, res, 0) == CELL_OK);
            results[i] = f.r64(res);
            done[i] = 1;
        });
        REQUIRE(eventually([&] { return queued<EventFlag>(*f.kernel, e) == static_cast<usize>(i + 1); }));
    }
    REQUIRE(c.call(kFlagSet, e, 0b11) == CELL_OK);
    const int first = protocol == SYS_SYNC_FIFO ? 0 : 1;
    const int second = 1 - first;
    REQUIRE(eventually([&] { return done[first].load() && done[2].load(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(done[second].load() == 0); // bit 0 was cleared by the first
    REQUIRE(c.call(kFlagSet, e, 0b01) == CELL_OK);
    for (auto& t : pool) t.join();
    CHECK(results[first] == 0b11);
    CHECK(results[2] == 0b10); // tested after the first waiter cleared bit 0
    CHECK(results[second] == 0b11);
}

TEST_CASE("lv2 event flag: one set wakes twenty waiters in protocol order, each seeing the pattern its turn left (D-246)",
    "[lv2][event_flag]")
{
    // More waiters than set() keeps on its stack, and one it leaves queued.
    const auto protocol = GENERATE(SYS_SYNC_FIFO, SYS_SYNC_PRIORITY);
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 e = make_flag(f, c, 0, protocol);
    constexpr int kWaiters = 21;
    std::atomic<int> done[kWaiters] = {};
    u64 results[kWaiters] = {};
    std::vector<std::thread> pool;
    for (int i = 0; i < kWaiters; i++) {
        pool.emplace_back([&, i] {
            Caller me(*f.kernel, "waiter", 1000 - i); // the later, the higher the priority
            const u32 res = f.alloc(8);
            const s32 r = me.call(kFlagWait, e, u64{1} << i, SYS_EVENT_FLAG_WAIT_OR | SYS_EVENT_FLAG_WAIT_CLEAR, res, 0);
            results[i] = f.r64(res);
            done[i] = r == CELL_OK ? 1 : 2;
        });
        REQUIRE(eventually([&] { return queued<EventFlag>(*f.kernel, e) == static_cast<usize>(i + 1); }));
    }
    constexpr u64 kAll = (u64{1} << (kWaiters - 1)) - 1; // every bit but the last waiter's
    REQUIRE(c.call(kFlagSet, e, kAll) == CELL_OK);
    REQUIRE(eventually([&] {
        for (int i = 0; i < kWaiters - 1; i++)
            if (!done[i].load()) return false;
        return true;
    }));
    CHECK(queued<EventFlag>(*f.kernel, e) == 1);
    for (int i = 0; i < kWaiters - 1; i++) {
        INFO("waiter " << i);
        CHECK(done[i].load() == 1);
        // FIFO tests waiter 0 first; priority the last one first. Each clears its own bit before the next is tested.
        const u64 seen = protocol == SYS_SYNC_FIFO ? kAll & ~((u64{1} << i) - 1) : kAll & ((u64{1} << (i + 1)) - 1);
        CHECK(results[i] == seen);
    }
    const u32 n = f.alloc(4);
    REQUIRE(c.call(kFlagCancel, e, n) == CELL_OK);
    CHECK(f.r32(n) == 1);
    for (auto& t : pool) t.join();
    CHECK(done[kWaiters - 1].load() == 2);
}

TEST_CASE("lv2 event flag: single-waiter flags, timeouts, cancel, destroy", "[lv2][event_flag]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 single = make_flag(f, c, 0, SYS_SYNC_FIFO, SYS_SYNC_WAITER_SINGLE);
    std::thread first([&] {
        Caller me(*f.kernel, "first");
        THREAD_CHECK(me.call(kFlagWait, single, 1, SYS_EVENT_FLAG_WAIT_AND, 0, 0) == CELL_OK);
    });
    REQUIRE(eventually([&] { return queued<EventFlag>(*f.kernel, single) == 1; }));
    const u32 res = f.alloc(8);
    CHECK(c.call(kFlagWait, single, 2, SYS_EVENT_FLAG_WAIT_AND, res, 0) == rc(CELL_EPERM));
    CHECK(c.call(kFlagDestroy, single) == rc(CELL_EBUSY));
    REQUIRE(c.call(kFlagSet, single, 1) == CELL_OK);
    first.join();

    const u32 e = make_flag(f, c, 0b100);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(c.call(kFlagWait, e, 0b011, SYS_EVENT_FLAG_WAIT_AND, res, 15000) == rc(CELL_ETIMEDOUT));
    CHECK(elapsed_ms(t0) >= 14);
    CHECK(f.r64(res) == 0b100); // the pattern at the timeout

    std::vector<std::thread> pool;
    for (int i = 0; i < 2; i++) {
        pool.emplace_back([&] {
            Caller me(*f.kernel, "waiter");
            const u32 r = f.alloc(8);
            THREAD_CHECK(me.call(kFlagWait, e, 0b1000, SYS_EVENT_FLAG_WAIT_OR, r, 0) == rc(CELL_ECANCELED));
            THREAD_CHECK(f.r64(r) == 0b100);
        });
    }
    REQUIRE(eventually([&] { return queued<EventFlag>(*f.kernel, e) == 2; }));
    const u32 num = f.alloc(4);
    REQUIRE(c.call(kFlagCancel, e, num) == CELL_OK);
    CHECK(f.r32(num) == 2);
    for (auto& t : pool) t.join();
    CHECK(c.call(kFlagDestroy, e) == CELL_OK);
}

TEST_CASE("lv2 event flag: handshakes between thread pairs", "[lv2][event_flag][stress]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 request = make_flag(f, c, 0);
    const u32 reply = make_flag(f, c, 0);
    constexpr int kPairs = 4, kRounds = 1500;
    constexpr u32 kAndClear = SYS_EVENT_FLAG_WAIT_AND | SYS_EVENT_FLAG_WAIT_CLEAR;
    std::atomic<int> served{0};
    std::vector<std::thread> pool;
    for (int i = 0; i < kPairs; i++) {
        const u64 bit = u64{1} << i;
        pool.emplace_back([&, bit] {
            Caller me(*f.kernel, "server");
            for (int r = 0; r < kRounds; r++) {
                THREAD_REQUIRE(me.call(kFlagWait, request, bit, kAndClear, 0, 0) == CELL_OK);
                served++;
                THREAD_REQUIRE(me.call(kFlagSet, reply, bit) == CELL_OK);
            }
        });
        pool.emplace_back([&, bit] {
            Caller me(*f.kernel, "client");
            for (int r = 0; r < kRounds; r++) {
                THREAD_REQUIRE(me.call(kFlagSet, request, bit) == CELL_OK);
                THREAD_REQUIRE(me.call(kFlagWait, reply, bit, kAndClear, 0, 0) == CELL_OK);
            }
        });
    }
    for (auto& t : pool) t.join();
    CHECK(served.load() == kPairs * kRounds);
    const u32 pat = f.alloc(8);
    REQUIRE(c.call(kFlagGet, request, pat) == CELL_OK);
    CHECK(f.r64(pat) == 0);
}

// D-999: a PPU thread woken by another PPU thread's event, of the same or a lower priority, goes back to guest code
// only once the sender has begun its next call into lv2 or a library, waits in lv2, or is gone (10 ms at most), and
// before it destroys an event queue or creates a thread the sender goes on until it waits (20 ms at most), as lv2's
// scheduler leaves the sender running on the PS3. Assassin's Creed's main thread, woken by a worker's "done",
// otherwise found the worker's object still busy and never started the next one (its first-install freeze), or tore
// the object down while the worker was still taking it out of the game's thread registry. The sender here is a host
// thread the host does not run (it sleeps) between its send and its next call.
TEST_CASE("lv2 event queue: the sending PPU thread goes on first (D-999)", "[lv2][event]")
{
    using namespace std::chrono_literals;
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 q = make_queue(f, c, 1);
    const u32 p = make_port(f, c);
    REQUIRE(c.call(kPortConnectLocal, p, q) == CELL_OK);
    const u32 other = make_queue(f, c, 1); // where the sender waits next (20 ms, nothing comes)
    const u32 events = f.alloc(32), number = f.alloc(4);

    // A blocked receiver of the same or a lower priority comes back once the sender begins its next call.
    for (s32 prio : {1000, 1500}) {
        std::atomic<bool> past{false}, saw_past{false};
        std::thread receiver([&] {
            Caller me(*f.kernel, "receiver", prio);
            THREAD_REQUIRE(me.call(kQueueReceive, q, 0, 0) == CELL_OK);
            saw_past = past.load();
        });
        REQUIRE(eventually([&] { return queued<EventQueue>(*f.kernel, q) == 1; }));
        std::thread sender([&] {
            Caller me(*f.kernel, "sender", 1000);
            THREAD_REQUIRE(me.call(kPortSend, p, 1, 2, 3) == CELL_OK);
            std::this_thread::sleep_for(3ms); // not running: the receiver must not overtake it
            past = true;
            THREAD_REQUIRE(me.call(kQueueTryReceive, other, events, 1, number) == CELL_OK);
        });
        sender.join();
        receiver.join();
        CHECK(saw_past);
    }

    // An event the sender queued, taken soon after: the same.
    {
        std::atomic<bool> past{false}, sent{false};
        std::thread sender([&] {
            Caller me(*f.kernel, "sender", 1000);
            THREAD_REQUIRE(me.call(kPortSend, p, 1, 2, 3) == CELL_OK);
            sent = true;
            std::this_thread::sleep_for(3ms);
            past = true;
            THREAD_CHECK(me.call(kQueueReceive, other, 0, 20000) == rc(CELL_ETIMEDOUT));
        });
        REQUIRE(eventually([&] { return sent.load(); }));
        REQUIRE(c.call(kQueueReceive, q, 0, 0) == CELL_OK);
        const bool saw_past = past.load();
        sender.join();
        CHECK(saw_past);
    }

    // A more urgent receiver does not wait (on the PS3 it would preempt the sender).
    {
        std::atomic<bool> past{false}, saw_past{true};
        std::thread receiver([&] {
            Caller me(*f.kernel, "receiver", 500);
            THREAD_REQUIRE(me.call(kQueueReceive, q, 0, 0) == CELL_OK);
            saw_past = past.load();
        });
        REQUIRE(eventually([&] { return queued<EventQueue>(*f.kernel, q) == 1; }));
        std::thread sender([&] {
            Caller me(*f.kernel, "sender", 1000);
            THREAD_REQUIRE(me.call(kPortSend, p, 1, 2, 3) == CELL_OK);
            std::this_thread::sleep_for(300ms);
            past = true;
        });
        receiver.join();
        CHECK_FALSE(saw_past);
        sender.join();
    }

    // Destroying a queue after the wake: the sender first goes on past its next call until it waits.
    {
        const u32 doomed = make_queue(f, c, 1);
        std::atomic<bool> past{false}, saw_past{false};
        std::thread receiver([&] {
            Caller me(*f.kernel, "receiver", 1000);
            THREAD_REQUIRE(me.call(kQueueReceive, q, 0, 0) == CELL_OK);
            THREAD_REQUIRE(me.call(kQueueDestroy, doomed, 0) == CELL_OK);
            saw_past = past.load();
        });
        REQUIRE(eventually([&] { return queued<EventQueue>(*f.kernel, q) == 1; }));
        std::thread sender([&] {
            Caller me(*f.kernel, "sender", 1000);
            THREAD_REQUIRE(me.call(kPortSend, p, 1, 2, 3) == CELL_OK);
            THREAD_REQUIRE(me.call(kQueueTryReceive, other, events, 1, number) == CELL_OK); // its next call
            std::this_thread::sleep_for(3ms);
            past = true;
            THREAD_CHECK(me.call(kQueueReceive, other, 0, 20000) == rc(CELL_ETIMEDOUT));
        });
        sender.join();
        receiver.join();
        CHECK(saw_past);
    }

    // A sender that neither calls nor waits holds its receiver 10 ms at most.
    {
        const auto t0 = std::chrono::steady_clock::now();
        s64 took = -1;
        std::thread receiver([&] {
            Caller me(*f.kernel, "receiver", 1000);
            THREAD_REQUIRE(me.call(kQueueReceive, q, 0, 0) == CELL_OK);
            took = elapsed_ms(t0);
        });
        REQUIRE(eventually([&] { return queued<EventQueue>(*f.kernel, q) == 1; }));
        std::thread sender([&] {
            Caller me(*f.kernel, "sender", 1000);
            THREAD_REQUIRE(me.call(kPortSend, p, 1, 2, 3) == CELL_OK);
            std::this_thread::sleep_for(500ms);
        });
        receiver.join();
        CHECK(took >= 0);
        CHECK(took < 300);
        sender.join();
    }
    const SenderDeferrals d = sender_deferrals();
    CHECK(d.deferred >= 5);
    CHECK(d.by_calls + d.by_wait >= 4); // a sender gone at once counts as waiting
    CHECK(d.by_calls >= 1);
    CHECK(d.by_limit >= 1);
    CHECK(d.barriers >= 1);
}
