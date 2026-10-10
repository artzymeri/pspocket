// SPURS queues (CellSpursQueue) as libsre has them (runtime/hle/src/cellSpursQueue.cpp): _cellSpursQueueInitialize's
// checks and the queue it writes, the lv2 event queue attach and detach, and cellSpursQueuePopBody against SPU tasks
// that push the way the SPU-side queue library does: first on guest memory from the host (two atomic updates of the
// queue line, the entry written between them, the PPU's wake-up thrown on the queue's SPU port), then a synthetic SPU
// task (assembled here: no game data) on the native scheduler, with each side waking the other.

#include "hle_test_util.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "pspocket/core/reservation.h"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/spu/context.h"
#include "guest.hpp" // runtime/hle/src: reservation_op
#include "spurs.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::spu::as;

namespace {

constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kTsAttr = kData + 0x4000, kPrio = kData + 0x4400;
constexpr u32 kTs = kData + 0x6000; // 6400 bytes
constexpr u32 kQueue = kData + 0x8000, kQueue2 = kData + 0x8080, kQueue3 = kData + 0x8100, kFlag = kData + 0x8180;
constexpr u32 kFakeTs = kData + 0x8200; // a taskset with a bad workload ID
constexpr u32 kEntries = kData + 0x9000; // the queue's entries
constexpr u32 kOut = kData + 0xA000;     // what pops copy out
constexpr u32 kId = kData + 0xB000, kArg = kData + 0xB100, kPattern = kData + 0xB200, kReport = kData + 0xB300;
constexpr u32 kNumber = kData + 0xB400;
constexpr u32 kCtx = kData + 0xC000;     // 0xC00 bytes: 1 KB + one LS block
constexpr u32 kQueues = kData + 0x10000; // up to 48 queues for the port test
constexpr u32 kElf = kData + 0x20000;

constexpr u32 SPU2SPU = 0, SPU2PPU = 1, PPU2SPU = 2;
const u64 kAgain = r3_of_code(0x80410901), kInval = r3_of_code(0x80410902), kPerm = r3_of_code(0x80410909),
          kBusy = r3_of_code(0x8041090A), kFault = r3_of_code(0x8041090D), kStat = r3_of_code(0x8041090F),
          kAlign = r3_of_code(0x80410910), kNull = r3_of_code(0x80410911), kFatal = r3_of_code(0x80410914);

bool wait_for(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// The queue's indices as cellSpursQueue.cpp reads them: ~index while a push or pop is under way.
bool marked(u32 v) { return static_cast<s32>(v) < 0; }
u32 unmarked(u32 v) { return marked(v) ? ~v : v; }
u32 entries(u32 pop, u32 push, u32 depth) { return pop <= push ? push - pop : push - pop + 2 * depth; }

// One SPURS instance with one SPU and a taskset (priority 1 on SPU 0, max contention 1). The native scheduler's workers
// run only when asked: the PPU API tests set task states by hand.
struct QueueFixture {
    Fixture f;
    GuestThread a{f, 0, "ppu"};
    u8* b = f.base();

    static hle::ProcessConfig config(bool workers)
    {
        hle::ProcessConfig c;
        c.spurs_workers = workers;
        return c;
    }

    explicit QueueFixture(bool workers = false) : f(config(workers))
    {
        REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 1, 200, 2016, 0) == 0);
        REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
        const u8 prio[8] = {1, 0, 0, 0, 0, 0, 0, 0};
        std::memcpy(b + kPrio, prio, 8);
        REQUIRE(a.call("_cellSpursTasksetAttributeInitialize", kTsAttr, 1, 0x330000, 0x1234, kPrio, 1) == 0);
        REQUIRE(a.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs, kTsAttr) == 0);
    }
    ~QueueFixture() { CHECK(a.call("cellSpursFinalize", kSpurs) == 0); }

    // An SPU2PPU queue of the taskset with `depth` entries of `size` bytes at kEntries, its lv2 event queue attached.
    void make_queue(u32 queue, u32 size, u32 depth, bool attach = true)
    {
        REQUIRE(a.call("_cellSpursQueueInitialize", 0, kTs, queue, kEntries, size, depth, SPU2PPU) == 0);
        if (attach) REQUIRE(a.call("cellSpursQueueAttachLv2EventQueue", queue) == 0);
    }
    u32 wid() const { return ps_read32(b, kTs + 0x74); }
    // The bit sets change with the workers' reservation operations: read through the reservation system, so what the
    // worker did before (a context saved) is visible with the bit (D-585).
    bool task_bit(u32 set_offset, u32 id) const
    {
        return line_word(b, kTs + set_offset + 4 * (id / 32)) & (0x80000000u >> (id % 32));
    }
};

// An entry's contents: byte j of the entry with value v.
u8 entry_byte(u32 v, u32 j) { return static_cast<u8>(v * 7 + j); }
bool entry_is(const u8* p, u32 v, u32 size)
{
    for (u32 j = 0; j < size; j++)
        if (p[j] != entry_byte(v, j)) return false;
    return true;
}

// sys_spu_thread_throw_event(port, 0, 0) from an SPU of `spurs`, delivered as the native scheduler's workers deliver
// it (spurs_scheduler.cpp, the interrupt mailbox): to the lv2 event queue the instance connected to the port.
void throw_event(Fixture& f, u32 spurs, u32 port)
{
    u32 queue_id = 0;
    {
        hle::spurs::SpursHost& h = hle::spurs::host();
        std::lock_guard lock(h.mutex);
        queue_id = h.instances.at(spurs).spu_ports.at(static_cast<u8>(port));
    }
    const auto q = f.kernel->event_queue(queue_id);
    REQUIRE(q != nullptr);
    REQUIRE(q->send(lv2::Event{0xFFFFFFFF53505501ull, 0, u64{port} << 32, 0}) == lv2::CELL_OK);
}

// An SPU task's push as the SPU-side queue library does it, on guest memory: step 1 claims the entry at the push
// index (stores ~index) unless the queue is full, another push is under way or tasks wait to push; the entry is
// written (the task's DMA); step 2 stores index + 1 (wrapping at 2 * depth) and, when the PPU's pop index is marked
// (a PopBody under way), the queue was empty and an lv2 event queue is attached, throws the PPU's wake-up event.
struct SpuPush {
    u8* b;
    u32 queue;
    u32 index = 0;

    bool claim()
    {
        bool ok = false;
        hle::reservation_op(b, queue, [&](u8* q) {
            ok = false;
            const u32 push = ps_read32(q, 0x04), depth = ps_read32(q, 0x0C);
            if (marked(push) || ps_read8(q, 0x30) || entries(unmarked(ps_read32(q, 0x00)), push, depth) == depth) {
                return false;
            }
            index = push;
            ps_write32(q, 0x04, ~push);
            ok = true;
            return true;
        });
        return ok;
    }
    void write(u32 value)
    {
        const u32 size = ps_read32(b, queue + 0x08), depth = ps_read32(b, queue + 0x0C);
        const u32 entry = static_cast<u32>(ps_read64(b, queue + 0x10)) + (index % depth) * size;
        for (u32 j = 0; j < size; j++) b[entry + j] = entry_byte(value, j);
    }
    // Returns the port to throw the PPU's wake-up event on, or 0xFFFFFFFF.
    u32 finish()
    {
        u32 port = 0xFFFFFFFF;
        hle::reservation_op(b, queue, [&](u8* q) {
            const u32 push = ~ps_read32(q, 0x04), pop = ps_read32(q, 0x00), depth = ps_read32(q, 0x0C);
            const bool wake = marked(pop) && entries(unmarked(pop), push, depth) == 0;
            port = wake ? ps_read32(q, 0x18) : 0xFFFFFFFF;
            ps_write32(q, 0x04, push + 1 < 2 * depth ? push + 1 : 0);
            return true;
        });
        return port;
    }
};

// cellSpursQueuePopBody on a test thread other than Catch2's (whose assertions aren't thread-safe, so no
// GuestThread::call): the function is looked up on the main thread.
u64 pop_on(GuestThread& t, const hle::Function& pop, u32 queue, u32 out, u32 peek, u32 blocking)
{
    t.ctx.gpr[3] = queue;
    t.ctx.gpr[4] = out;
    t.ctx.gpr[5] = peek;
    t.ctx.gpr[6] = blocking;
    pop.fn(t.ctx, t.f.base());
    return t.ctx.gpr[3];
}

// A whole push; false (nothing changed) when the task would have to wait.
bool spu_push(QueueFixture& s, u32 queue, u32 value)
{
    SpuPush p{s.b, queue};
    if (!p.claim()) return false;
    p.write(value);
    if (const u32 port = p.finish(); port != 0xFFFFFFFF) throw_event(s.f, kSpurs, port);
    return true;
}

// What an SPU task waiting to push leaves in the queue: its task ID and workload ID appended to the waiter list.
void add_push_waiter(u8* b, u32 queue, u8 task, u8 wid)
{
    hle::reservation_op(b, queue, [&](u8* q) {
        const u8 n = ps_read8(q, 0x30);
        ps_write8(q, 0x31 + n, task);
        ps_write8(q, 0x30, static_cast<u8>(n + 1));
        ps_write8(q, 0x51 + n, wid);
        ps_write8(q, 0x50, static_cast<u8>(n + 1));
        return true;
    });
}

// An SPU task's pop as the SPU-side queue library does it (the mirror of SpuPush, for PPU2SPU queues): step 1 claims the
// entry at the pop index unless the queue is empty, another pop is under way or tasks wait to pop; the entry is read;
// step 2 stores index + 1 and, when the PPU's push index is marked (a PushBody under way) and the queue was full,
// returns the port to throw the PPU's wake-up event on.
struct SpuPop {
    u8* b;
    u32 queue;
    u32 index = 0;

    bool claim()
    {
        bool ok = false;
        hle::reservation_op(b, queue, [&](u8* q) {
            ok = false;
            const u32 pop = ps_read32(q, 0x00), depth = ps_read32(q, 0x0C);
            if (marked(pop) || ps_read8(q, 0x20) || entries(pop, unmarked(ps_read32(q, 0x04)), depth) == 0) return false;
            index = pop;
            ps_write32(q, 0x00, ~pop);
            ok = true;
            return true;
        });
        return ok;
    }
    bool read_is(u32 value) const
    {
        const u32 size = ps_read32(b, queue + 0x08), depth = ps_read32(b, queue + 0x0C);
        return entry_is(b + static_cast<u32>(ps_read64(b, queue + 0x10)) + (index % depth) * size, value, size);
    }
    u32 finish()
    {
        u32 port = 0xFFFFFFFF;
        hle::reservation_op(b, queue, [&](u8* q) {
            const u32 pop = ~ps_read32(q, 0x00), push = ps_read32(q, 0x04), depth = ps_read32(q, 0x0C);
            const bool wake = marked(push) && entries(pop, unmarked(push), depth) == depth;
            port = wake ? ps_read32(q, 0x18) : 0xFFFFFFFF;
            ps_write32(q, 0x00, pop + 1 < 2 * depth ? pop + 1 : 0);
            return true;
        });
        return port;
    }
};

// A whole SPU pop of an entry expected to hold `value`; false (nothing changed) when the task would have to wait.
bool spu_pop(QueueFixture& s, u32 queue, u32 value)
{
    SpuPop p{s.b, queue};
    if (!p.claim()) return false;
    CHECK(p.read_is(value));
    if (const u32 port = p.finish(); port != 0xFFFFFFFF) throw_event(s.f, kSpurs, port);
    return true;
}

// What an SPU task waiting to pop leaves in the queue: its task ID and workload ID appended to the waiter list.
void add_pop_waiter(u8* b, u32 queue, u8 task, u8 wid)
{
    hle::reservation_op(b, queue, [&](u8* q) {
        const u8 n = ps_read8(q, 0x20);
        ps_write8(q, 0x21 + n, task);
        ps_write8(q, 0x20, static_cast<u8>(n + 1));
        ps_write8(q, 0x41 + n, wid);
        ps_write8(q, 0x40, static_cast<u8>(n + 1));
        return true;
    });
}

// The PPU's input for a push of `value`.
void put_entry(u8* b, u32 at, u32 value, u32 size)
{
    for (u32 j = 0; j < size; j++) b[at + j] = entry_byte(value, j);
}

// What the oldest waiter does when it runs again after its signal: it leaves the list (the others move up).
void remove_push_waiter(u8* b, u32 queue)
{
    hle::reservation_op(b, queue, [&](u8* q) {
        std::memmove(q + 0x31, q + 0x32, 14);
        std::memmove(q + 0x51, q + 0x52, 14);
        q[0x3F] = q[0x5F] = 0;
        ps_write8(q, 0x30, static_cast<u8>(ps_read8(q, 0x30) - 1));
        ps_write8(q, 0x50, static_cast<u8>(ps_read8(q, 0x50) - 1));
        return true;
    });
}

} // namespace

TEST_CASE("SPURS queues: _cellSpursQueueInitialize as libsre, checks in its order", "[hle][spurs][spurs-queue]")
{
    QueueFixture s;
    u8* b = s.b;
    auto init = [&](u32 spurs, u32 ts, u32 q, u32 buf, u32 size, u32 depth, u32 dir) {
        return s.a.call("_cellSpursQueueInitialize", spurs, ts, q, buf, size, depth, dir);
    };

    // GTA V's call: no SPURS, a taskset, 16-byte entries, depth 128, SPU2PPU. 0x70.. is left alone.
    std::memset(b + kQueue, 0xAA, 128);
    REQUIRE(init(0, kTs, kQueue, kEntries, 16, 128, SPU2PPU) == 0);
    CHECK(ps_read32(b, kQueue + 0x00) == 0);          // pop index
    CHECK(ps_read32(b, kQueue + 0x04) == 0);          // push index
    CHECK(ps_read32(b, kQueue + 0x08) == 16);         // entry size
    CHECK(ps_read32(b, kQueue + 0x0C) == 128);        // depth
    CHECK(ps_read64(b, kQueue + 0x10) == kEntries);   // the entries
    CHECK(ps_read32(b, kQueue + 0x18) == 0xFFFFFFFF); // no SPU port
    CHECK(ps_read32(b, kQueue + 0x1C) == SPU2PPU);
    for (u32 off = 0x20; off < 0x60; off++) CHECK(b[kQueue + off] == 0); // no waiters
    CHECK(ps_read64(b, kQueue + 0x60) == kTs);
    CHECK(ps_read64(b, kQueue + 0x68) == kSpurs);     // the taskset's CellSpurs
    for (u32 off = 0x70; off < 0x80; off++) CHECK(b[kQueue + off] == 0xAA);

    // A queue of the whole instance (cellSpursQueueInitializeIWL); no buffer is needed for entries of 0 bytes.
    REQUIRE(init(kSpurs, 0, kQueue2, 0, 0, 1, PPU2SPU) == 0);
    CHECK(ps_read64(b, kQueue2 + 0x60) == 0);
    CHECK(ps_read64(b, kQueue2 + 0x68) == kSpurs);
    CHECK(ps_read32(b, kQueue2 + 0x1C) == PPU2SPU);
    // With both, the taskset decides; the SPURS pointer is only checked for alignment.
    REQUIRE(init(kSpurs + 0x80000, kTs, kQueue2, kEntries, 0x4000, 0x3FFFFFFF, SPU2SPU) == 0);
    CHECK(ps_read64(b, kQueue2 + 0x68) == kSpurs);

    // NULL_POINTER first, then ALIGN (spurs, taskset, queue, buffer), then INVAL.
    CHECK(init(0, 0, kQueue, kEntries, 16, 4, SPU2PPU) == kNull);
    CHECK(init(0, kTs, 0, kEntries, 16, 4, SPU2PPU) == kNull);
    CHECK(init(0, kTs, kQueue, 0, 16, 4, SPU2PPU) == kNull);
    CHECK(init(kSpurs + 8, kTs, 0, kEntries, 16, 4, SPU2PPU) == kNull);
    CHECK(init(kSpurs + 8, kTs, kQueue, kEntries, 16, 4, SPU2PPU) == kAlign);
    CHECK(init(0, kTs + 0x40, kQueue, kEntries, 16, 4, SPU2PPU) == kAlign);
    CHECK(init(0, kTs, kQueue + 0x40, kEntries, 16, 4, SPU2PPU) == kAlign);
    CHECK(init(0, kTs, kQueue, kEntries + 8, 16, 4, SPU2PPU) == kAlign);
    CHECK(init(0, kTs, kQueue + 0x40, kEntries, 17, 0, 9) == kAlign);
    std::memset(b + kFakeTs, 0, 0x80);
    ps_write32(b, kFakeTs + 0x74, 32); // workload ID
    CHECK(init(0, kFakeTs, kQueue, kEntries, 16, 4, SPU2PPU) == kInval);
    ps_write32(b, kFakeTs + 0x74, 31);
    ps_write64(b, kFakeTs + 0x60, 0x12345680);
    REQUIRE(init(0, kFakeTs, kQueue3, kEntries, 16, 4, SPU2PPU) == 0);
    CHECK(ps_read64(b, kQueue3 + 0x68) == 0x12345680); // taken as it is
    CHECK(init(0, kTs, kQueue, kEntries, 0x4010, 4, SPU2PPU) == kInval); // more than 16 KB
    CHECK(init(0, kTs, kQueue, kEntries, 24, 4, SPU2PPU) == kInval);     // not a multiple of 16
    CHECK(init(0, kTs, kQueue, kEntries, 16, 0, SPU2PPU) == kInval);
    CHECK(init(0, kTs, kQueue, kEntries, 16, 0x40000000, SPU2PPU) == kInval);
    CHECK(init(0, kTs, kQueue, kEntries, 16, 4, 3) == kInval);
    // Pointers libsre would just read: refused instead (D-042).
    CHECK(init(0, 0x40000000, kQueue, kEntries, 16, 4, SPU2PPU) == kFault);
    CHECK(init(0, kTs, 0x40000000, kEntries, 16, 4, SPU2PPU) == kFault);
    CHECK(ps_read32(b, kQueue + 0x0C) == 128); // nothing written by the refused calls
}

TEST_CASE("SPURS queues: the lv2 event queue attach and detach as libsre", "[hle][spurs][spurs-queue]")
{
    QueueFixture s;
    u8* b = s.b;
    s.make_queue(kQueue, 16, 4, false);
    CHECK(s.a.call("cellSpursQueueAttachLv2EventQueue", 0) == kNull);
    CHECK(s.a.call("cellSpursQueueAttachLv2EventQueue", kQueue + 0x10) == kAlign);
    CHECK(s.a.call("cellSpursQueueDetachLv2EventQueue", kQueue) == kStat); // nothing attached

    REQUIRE(s.a.call("cellSpursQueueAttachLv2EventQueue", kQueue) == 0);
    const u32 port = ps_read32(b, kQueue + 0x18);
    CHECK(port == 0x10); // the lowest dynamic SPU port
    const u32 equeue = static_cast<u32>(ps_read64(b, kQueue + 0x70));
    const auto q = s.f.kernel->event_queue(equeue);
    REQUIRE(q != nullptr);
    CHECK(q->name == 0x5F73707551756500ull); // "_spuQue"
    CHECK(q->protocol == lv2::SYS_SYNC_FIFO);
    CHECK(q->type == lv2::SYS_PPU_QUEUE);
    CHECK(q->size == 1);
    {
        hle::spurs::SpursHost& h = hle::spurs::host();
        std::lock_guard lock(h.mutex);
        CHECK(h.instances.at(kSpurs).spu_ports.at(0x10) == equeue); // where SPU events on the port go
    }
    CHECK(ps_read64(b, kSpurs + 0xDA8) == 0); // spuPortBits: SPURS' own queues don't show
    CHECK(s.a.call("cellSpursQueueAttachLv2EventQueue", kQueue) == kStat);

    // The next queue, and an event flag after it, get the next ports.
    s.make_queue(kQueue2, 16, 4);
    CHECK(ps_read32(b, kQueue2 + 0x18) == 0x11);
    REQUIRE(s.a.call("_cellSpursEventFlagInitialize", 0, kTs, kFlag, 0, 1 /* SPU2PPU */) == 0);
    REQUIRE(s.a.call("cellSpursEventFlagAttachLv2EventQueue", kFlag) == 0);
    CHECK(ps_read8(b, kFlag + 0x0C) == 0x12);

    // Only queues the PPU pushes to or pops from take an lv2 queue, and only with a CellSpurs (0x68).
    REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, kQueue3, kEntries, 16, 4, SPU2SPU) == 0);
    CHECK(s.a.call("cellSpursQueueAttachLv2EventQueue", kQueue3) == kPerm);
    REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, kQueue3, kEntries, 16, 4, PPU2SPU) == 0);
    ps_write64(b, kQueue3 + 0x68, 0);
    CHECK(s.a.call("cellSpursQueueAttachLv2EventQueue", kQueue3) == kInval);

    // Detach: BUSY while the PPU's side is marked (a PopBody of an SPU2PPU queue, a push of PPU2SPU).
    CHECK(s.a.call("cellSpursQueueDetachLv2EventQueue", 0) == kNull);
    CHECK(s.a.call("cellSpursQueueDetachLv2EventQueue", kQueue + 0x40) == kAlign);
    ps_write32(b, kQueue + 0x00, ~0u);
    CHECK(s.a.call("cellSpursQueueDetachLv2EventQueue", kQueue) == kBusy);
    ps_write32(b, kQueue + 0x00, 0);
    ps_write32(b, kQueue + 0x04, ~3u); // an SPU push under way doesn't matter to the PPU
    REQUIRE(s.a.call("cellSpursQueueDetachLv2EventQueue", kQueue) == 0);
    CHECK(ps_read32(b, kQueue + 0x18) == 0xFFFFFFFF);
    CHECK(s.f.kernel->event_queue(equeue) == nullptr); // destroyed
    {
        hle::spurs::SpursHost& h = hle::spurs::host();
        std::lock_guard lock(h.mutex);
        CHECK_FALSE(h.instances.at(kSpurs).spu_ports.contains(0x10));
    }
    CHECK(s.a.call("cellSpursQueueDetachLv2EventQueue", kQueue) == kStat);
    REQUIRE(s.a.call("cellSpursQueueAttachLv2EventQueue", kQueue) == 0);
    CHECK(ps_read32(b, kQueue + 0x18) == 0x10); // free again
    REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, kQueue3, kEntries, 16, 4, PPU2SPU) == 0);
    REQUIRE(s.a.call("cellSpursQueueAttachLv2EventQueue", kQueue3) == 0);
    ps_write32(b, kQueue3 + 0x04, ~1u); // the PPU's push under way
    CHECK(s.a.call("cellSpursQueueDetachLv2EventQueue", kQueue3) == kBusy);
    ps_write32(b, kQueue3 + 0x04, 1);
    const u64 spurs3 = ps_read64(b, kQueue3 + 0x68);
    ps_write64(b, kQueue3 + 0x68, 0);
    ps_write64(b, kQueue3 + 0x60, 0);
    CHECK(s.a.call("cellSpursQueueDetachLv2EventQueue", kQueue3) == kInval);
    ps_write64(b, kQueue3 + 0x68, spurs3);
    REQUIRE(s.a.call("cellSpursQueueDetachLv2EventQueue", kQueue3) == 0);

    // No free SPU port: the SPURS error comes back as the task error with the same code (CORE BUSY -> TASK BUSY), and
    // the lv2 queue made for the attach is destroyed again.
    u32 attached = 0;
    for (u32 i = 0; i < 48; i++) {
        const u32 qi = kQueues + 0x80 * i;
        REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, qi, kEntries, 16, 4, SPU2PPU) == 0);
        if (s.a.call("cellSpursQueueAttachLv2EventQueue", qi) != 0) break;
        attached++;
    }
    CHECK(attached == 48 - 3); // ports 0x10..0x3F, three of them taken above
    lv2::sys_process_get_number_of_object(*s.a.t, 0x8D /* SYS_EVENT_QUEUE_OBJECT */, kNumber);
    const u32 queues_before = ps_read32(b, kNumber);
    const u32 last = kQueues + 0x80 * attached;
    CHECK(s.a.call("cellSpursQueueAttachLv2EventQueue", last) == kBusy);
    CHECK(ps_read32(b, last + 0x18) == 0xFFFFFFFF);
    lv2::sys_process_get_number_of_object(*s.a.t, 0x8D, kNumber);
    CHECK(ps_read32(b, kNumber) == queues_before);
}

TEST_CASE("SPURS queues: cellSpursQueuePopBody checks, entries in order, peek, the indices' wrap",
    "[hle][spurs][spurs-queue]")
{
    QueueFixture s;
    u8* b = s.b;
    constexpr u32 kSize = 32, kDepth = 4;
    s.make_queue(kQueue, kSize, kDepth);
    auto pop = [&](u32 out, u32 peek, u32 blocking) {
        return s.a.call("cellSpursQueuePopBody", kQueue, out, peek, blocking);
    };

    CHECK(s.a.call("cellSpursQueuePopBody", 0, kOut, 0, 0) == kNull);
    CHECK(pop(0, 0, 0) == kNull);
    CHECK(s.a.call("cellSpursQueuePopBody", kQueue + 0x40, kOut, 0, 0) == kAlign);
    REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, kQueue2, kEntries, kSize, kDepth, PPU2SPU) == 0);
    CHECK(s.a.call("cellSpursQueuePopBody", kQueue2, kOut, 0, 0) == kPerm); // the PPU pushes to PPU2SPU queues
    REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, kQueue2, kEntries, kSize, kDepth, SPU2SPU) == 0);
    CHECK(s.a.call("cellSpursQueuePopBody", kQueue2, kOut, 0, 1) == kPerm);
    REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, kQueue2, kEntries + 0x800, kSize, kDepth, SPU2PPU) == 0);
    auto pop2 = [&](u32 out, u32 peek, u32 blocking) {
        return s.a.call("cellSpursQueuePopBody", kQueue2, out, peek, blocking);
    };
    CHECK(pop2(kOut, 0, 1) == kStat);        // blocking needs the lv2 queue
    CHECK(pop2(kOut, 0, 0) == kAgain);       // empty: TryPop doesn't need it
    CHECK(pop2(0x40000000, 0, 0) == kAgain); // libsre copies nothing out of an empty queue
    REQUIRE(spu_push(s, kQueue2, 11));
    CHECK(pop2(0x40000000, 0, 0) == kFault); // refused before the entry is claimed (D-042)
    CHECK(ps_read32(b, kQueue2) == 0);
    REQUIRE(pop2(kOut, 0, 0) == 0);
    CHECK(entry_is(b + kOut, 11, kSize));
    CHECK(pop(kOut, 0, 0) == kAgain);
    CHECK(pop(kOut, 1, 0) == kAgain);
    CHECK(ps_read32(b, kQueue) == 0); // AGAIN leaves the line as it was

    // Three entries pushed the SPU way come out in order; a peek leaves the entry in the queue.
    for (u32 v = 1; v <= 3; v++) REQUIRE(spu_push(s, kQueue, v));
    CHECK(ps_read32(b, kQueue + 0x04) == 3);
    REQUIRE(pop(kOut, 1, 0) == 0);
    CHECK(entry_is(b + kOut, 1, kSize));
    CHECK(ps_read32(b, kQueue) == 0);
    REQUIRE(pop(kOut + 0x40, 1 /* only the low byte counts */ | 0x100, 0x100) == 0); // TryPeek
    CHECK(entry_is(b + kOut + 0x40, 1, kSize));
    for (u32 v = 1; v <= 3; v++) {
        std::memset(b + kOut, 0, kSize);
        REQUIRE(pop(kOut, 0, v & 1) == 0); // Pop and TryPop take the same path while there are entries
        CHECK(entry_is(b + kOut, v, kSize));
        CHECK(ps_read32(b, kQueue) == v);
    }
    CHECK(pop(kOut, 0, 0) == kAgain);

    // Full is depth entries; the indices run to 2 * depth and wrap, an entry's slot is its index mod depth.
    for (u32 v = 4; v <= 7; v++) REQUIRE(spu_push(s, kQueue, v));
    CHECK_FALSE(spu_push(s, kQueue, 8)); // full
    CHECK(ps_read32(b, kQueue + 0x04) == 7);
    for (u32 v = 4; v <= 7; v++) {
        REQUIRE(pop(kOut, 0, 1) == 0);
        CHECK(entry_is(b + kOut, v, kSize));
        CHECK(entry_is(b + kEntries + ((v - 1) % kDepth) * kSize, v, kSize)); // entry v went to index v - 1
    }
    CHECK(ps_read32(b, kQueue) == 7);
    REQUIRE(spu_push(s, kQueue, 8)); // index 7, slot 3; the push index wraps to 0
    CHECK(ps_read32(b, kQueue + 0x04) == 0);
    REQUIRE(spu_push(s, kQueue, 9)); // index 0, slot 0
    REQUIRE(pop(kOut, 0, 1) == 0);
    CHECK(entry_is(b + kOut, 8, kSize));
    CHECK(ps_read32(b, kQueue) == 0); // the pop index wraps too
    REQUIRE(pop(kOut, 0, 0) == 0);
    CHECK(entry_is(b + kOut, 9, kSize));
    CHECK(ps_read32(b, kQueue) == 1);

    // Another PPU caller's pop under way (its index marked): BUSY, before the STAT of a missing lv2 queue.
    ps_write32(b, kQueue, ~1u);
    CHECK(pop(kOut, 0, 1) == kBusy);
    CHECK(pop(kOut, 0, 0) == kBusy);
    ps_write32(b, kQueue2, ~1u);
    CHECK(pop2(kOut, 0, 1) == kBusy);
    ps_write32(b, kQueue2, 1);
    ps_write32(b, kQueue, 1);
    // An SPU push under way: its entry is not there yet.
    SpuPush p{b, kQueue};
    REQUIRE(p.claim());
    CHECK(ps_read32(b, kQueue + 0x04) == ~1u);
    CHECK(pop(kOut, 0, 0) == kAgain);
    p.write(10);
    CHECK(p.finish() == 0xFFFFFFFF); // no PPU waits
    REQUIRE(pop(kOut, 0, 0) == 0);
    CHECK(entry_is(b + kOut, 10, kSize));
}

TEST_CASE("SPURS queues: a blocking pop waits for the SPU push's event on the queue's SPU port",
    "[hle][spurs][spurs-queue]")
{
    QueueFixture s;
    u8* b = s.b;
    constexpr u32 kSize = 16, kDepth = 2;
    s.make_queue(kQueue, kSize, kDepth);
    const u32 port = ps_read32(b, kQueue + 0x18);

    std::atomic<u64> rc{~0ull};
    const hle::Function& pop = GuestThread::find("cellSpursQueuePopBody");
    std::thread popper([&] {
        GuestThread p(s.f, 1, "popper");
        rc = pop_on(p, pop, kQueue, kOut, 0, 1);
    });
    // The PPU marks the pop index for its whole call and waits on the lv2 queue.
    REQUIRE(wait_for([&] { return line_word(b, kQueue) == ~0u; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(rc.load() == ~0ull);
    CHECK(s.a.call("cellSpursQueuePopBody", kQueue, kOut + 0x100, 0, 0) == kBusy); // one PPU caller at a time
    CHECK(s.a.call("cellSpursQueueDetachLv2EventQueue", kQueue) == kBusy);

    // An event while the queue is still empty: the PPU looks again and goes on waiting.
    throw_event(s.f, kSpurs, port);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(rc.load() == ~0ull);
    CHECK(ps_read32(b, kQueue) == ~0u);

    // The SPU push: step 2 sees the marked pop index of an empty queue and wakes the PPU through the port.
    SpuPush p{b, kQueue};
    REQUIRE(p.claim());
    p.write(42);
    REQUIRE(p.finish() == port);
    throw_event(s.f, kSpurs, port);
    popper.join();
    CHECK(rc.load() == 0);
    CHECK(entry_is(b + kOut, 42, kSize));
    CHECK(ps_read32(b, kQueue) == 1); // advanced, unmarked
    CHECK(ps_read32(b, kQueue + 0x04) == 1);

    // Entries already there: no wait, no event needed.
    REQUIRE(spu_push(s, kQueue, 43));
    REQUIRE(s.a.call("cellSpursQueuePopBody", kQueue, kOut, 0, 1) == 0);
    CHECK(entry_is(b + kOut, 43, kSize));
    REQUIRE(s.a.call("cellSpursQueueDetachLv2EventQueue", kQueue) == 0);
}

TEST_CASE("SPURS queues: a pop from a full queue signals the oldest SPU task waiting to push",
    "[hle][spurs][spurs-queue]")
{
    QueueFixture s; // no workers: the tasks' states are set here
    u8* b = s.b;
    constexpr u32 kSize = 16, kDepth = 2;
    s.make_queue(kQueue, kSize, kDepth);
    const u8 wid = static_cast<u8>(s.wid());
    // Tasks 3 and 5 exist and wait (their contexts saved); both are in the queue's push waiter list, 3 first.
    ps_write32(b, kTs + 0x30, (0x80000000u >> 3) | (0x80000000u >> 5)); // enabled
    ps_write32(b, kTs + 0x50, (0x80000000u >> 3) | (0x80000000u >> 5)); // waiting
    REQUIRE(spu_push(s, kQueue, 1));
    REQUIRE(spu_push(s, kQueue, 2));
    add_push_waiter(b, kQueue, 3, wid);
    add_push_waiter(b, kQueue, 5, wid);

    // A peek takes nothing: no signal.
    REQUIRE(s.a.call("cellSpursQueuePopBody", kQueue, kOut, 1, 1) == 0);
    CHECK_FALSE(s.task_bit(0x40, 3));
    // The pop frees an entry of the full queue: task 3 gets the signal, and its workload too (it was waiting).
    REQUIRE(s.a.call("cellSpursQueuePopBody", kQueue, kOut, 0, 1) == 0);
    CHECK(entry_is(b + kOut, 1, kSize));
    CHECK(s.task_bit(0x40, 3));        // signalled
    CHECK_FALSE(s.task_bit(0x40, 5));
    CHECK(ps_read16(b, kSpurs + 0x70) & (0x8000u >> wid)); // wklSignal1
    // The woken task takes itself off the list when it runs again; until then the queue is not full and the next pop
    // signals no one.
    REQUIRE(s.a.call("cellSpursQueuePopBody", kQueue, kOut, 0, 0) == 0);
    CHECK(entry_is(b + kOut, 2, kSize));
    CHECK_FALSE(s.task_bit(0x40, 5));
    CHECK(ps_read8(b, kQueue + 0x30) == 2); // the PPU never edits the list

    // A waiter's task ID libsre's _cellSpursSendSignal refuses with INVAL: the pop is done, the result is FATAL.
    REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, kQueue2, kEntries + 0x100, kSize, 1, SPU2PPU) == 0);
    REQUIRE(spu_push(s, kQueue2, 7));
    add_push_waiter(b, kQueue2, 200, wid);
    CHECK(s.a.call("cellSpursQueuePopBody", kQueue2, kOut, 0, 0) == kFatal);
    CHECK(entry_is(b + kOut, 7, kSize));
    CHECK(ps_read32(b, kQueue2) == 1);

    // A queue of the whole instance finds the waiter's taskset through its workload ID (WorkloadInfo.arg).
    REQUIRE(s.a.call("_cellSpursQueueInitialize", kSpurs, 0, kQueue3, kEntries + 0x200, kSize, 1, SPU2PPU) == 0);
    REQUIRE(spu_push(s, kQueue3, 8));
    add_push_waiter(b, kQueue3, 5, wid);
    REQUIRE(s.a.call("cellSpursQueuePopBody", kQueue3, kOut, 0, 0) == 0);
    CHECK(entry_is(b + kOut, 8, kSize));
    CHECK(s.task_bit(0x40, 5));
    // ... and refuses with FATAL when that workload doesn't exist (libsre signals an undefined taskset there).
    CHECK_FALSE(spu_push(s, kQueue3, 9)); // tasks wait: a new push waits behind them
    remove_push_waiter(b, kQueue3);       // task 5 runs again
    REQUIRE(spu_push(s, kQueue3, 9));
    add_push_waiter(b, kQueue3, 6, 9);    // workload 9: not registered
    CHECK(s.a.call("cellSpursQueuePopBody", kQueue3, kOut, 0, 0) == kFatal);
    CHECK(entry_is(b + kOut, 9, kSize));
    CHECK(ps_read32(b, kQueue3) == 0); // the indices of a one-entry queue wrap at 2
}

// ------------------------------------------------------------------------------------------------- an SPU task

namespace {

constexpr u32 kBuf = 0x3800; // the task's LS buffer for the entry and its report

// An SPU task program, assembled at 0x3000, with forward branches to labels.
struct Program {
    struct Fixup {
        usize at, label;
        int kind; // 0 br, 1 brz, 2 brnz
        u32 rt;
    };
    std::vector<u32> code;
    std::vector<usize> labels;
    std::vector<Fixup> fixups;

    void emit(u32 insn) { code.push_back(insn); }
    usize label()
    {
        labels.push_back(~usize{0});
        return labels.size() - 1;
    }
    void bind(usize l) { labels[l] = code.size(); }
    void br(usize l) { branch(0, 0, l); }
    void brz(u32 rt, usize l) { branch(1, rt, l); }
    void brnz(u32 rt, usize l) { branch(2, rt, l); }
    void branch(int kind, u32 rt, usize l)
    {
        fixups.push_back({code.size(), l, kind, rt});
        code.push_back(0);
    }
    void finish()
    {
        for (const Fixup& f : fixups) {
            const s32 off = (static_cast<s32>(labels[f.label]) - static_cast<s32>(f.at)) * 4;
            code[f.at] = f.kind == 0 ? as::br(off) : f.kind == 1 ? as::brz(f.rt, off) : as::brnz(f.rt, off);
        }
    }
    void load32(u32 rt, u32 v)
    {
        emit(as::ilhu(rt, static_cast<s32>(v >> 16)));
        emit(as::iohl(rt, static_cast<s32>(v & 0xFFFF)));
    }
    // A DMA of `size_reg` bytes between LS (`ls_reg`) and main memory (`ea_reg`), waited for (tag 1).
    void dma(u32 cmd, u32 ls_reg, u32 ea_reg, u32 size_reg)
    {
        constexpr u32 t = 70;
        emit(as::wrch(PS_MFC_LSA, ls_reg));
        emit(as::il(t, 0));
        emit(as::wrch(PS_MFC_EAH, t));
        emit(as::wrch(PS_MFC_EAL, ea_reg));
        emit(as::wrch(PS_MFC_Size, size_reg));
        emit(as::il(t, 1));
        emit(as::wrch(PS_MFC_TagID, t));
        emit(as::il(t, static_cast<s32>(cmd)));
        emit(as::wrch(PS_MFC_Cmd, t));
        emit(as::il(t, 2));
        emit(as::wrch(PS_MFC_WrTagMask, t));
        emit(as::il(t, PS_MFC_TAG_UPDATE_ALL));
        emit(as::wrch(PS_MFC_WrTagUpdate, t));
        emit(as::rdch(t, PS_MFC_RdTagStat));
    }
    // GETLLAR / PUTLLC of the line at EA `ea_reg` through LS 0x80 (where the SPU-side library keeps it); after a
    // PUTLLC, `status` is non-zero when it failed.
    void line(u32 cmd, u32 ea_reg, u32 status)
    {
        constexpr u32 t = 71;
        emit(as::il(t, 0x80));
        emit(as::wrch(PS_MFC_LSA, t));
        emit(as::il(t, 0));
        emit(as::wrch(PS_MFC_EAH, t));
        emit(as::wrch(PS_MFC_EAL, ea_reg));
        emit(as::il(t, static_cast<s32>(cmd)));
        emit(as::wrch(PS_MFC_Cmd, t));
        emit(as::rdch(status, PS_MFC_RdAtomicStat));
    }
    // The taskset policy's syscall entry, as the SPU-side task library calls it.
    void syscall(u32 n)
    {
        emit(as::il(3, static_cast<s32>(n)));
        emit(as::ila(5, 0xA70));
        emit(as::bisl(0, 5));
    }
    // `rt` = word `index` of the quadword `qw` replaced by `value`'s preferred word (zero: a register holding 0).
    void insert_word(u32 rt, u32 value, u32 qw, u32 index, u32 zero)
    {
        emit(as::cwd(72, zero, static_cast<s32>(4 * index)));
        emit(as::shufb(rt, value, qw, 72));
    }
    void insert_byte(u32 rt, u32 value, u32 qw, u32 index, u32 zero)
    {
        emit(as::cbd(72, zero, static_cast<s32>(index)));
        emit(as::shufb(rt, value, qw, 72));
    }
};

// A task that pushes entries to a SPURS queue as the SPU-side queue library does (written for this test from the
// protocol, one pusher). Argument: queue EA, number of entries, first value, report EA. Entry i is value + i in word 0
// and i in word 1. When the queue is full it adds itself to the (empty) waiter list, waits for a signal (syscall 2,
// wait object queue | 4 at LS 0x2FD8), and then takes the entry without checking again. Step 2 throws the PPU's
// wake-up event on the queue's SPU port when the PPU's pop index is marked and the queue was empty. At the end it
// reports {entries pushed, times it waited}.
Program pusher_task()
{
    Program p;
    // Kept across syscalls ($80..$127): queue, count, next value, waits, pushed, waited, report EA.
    constexpr u32 rQ = 80, rN = 81, rV = 82, rWaits = 83, rI = 84, rWaited = 85, rReport = 86;
    p.emit(as::ila(1, 0x3FFD0));
    p.emit(as::ori(rQ, 3, 0));
    p.emit(as::rotqbyi(rN, 3, 4));
    p.emit(as::rotqbyi(rV, 3, 8));
    p.emit(as::rotqbyi(rReport, 3, 12));
    p.emit(as::il(rWaits, 0));
    p.emit(as::il(rI, 0));
    p.emit(as::il(rWaited, 0));
    const usize next = p.label(), step1 = p.label(), wait = p.label(), claim = p.label(), mark = p.label(),
                step2 = p.label(), no_event = p.label(), done = p.label();

    p.bind(next);
    p.emit(as::ceq(10, rI, rN));
    p.brnz(10, done);

    // Step 1: claim the entry at the push index.
    p.bind(step1);
    p.line(PS_MFC_GETLLAR, rQ, 11);
    p.emit(as::lqa(20, 0x80));         // pop, push, size, depth
    p.emit(as::lqa(21, 0x90));         // entries (u64), SPU port, direction
    p.emit(as::rotqbyi(22, 20, 4));    // push
    p.emit(as::rotqbyi(23, 20, 12));   // depth
    p.brnz(rWaited, claim);            // woken by a pop that freed an entry: it is ours
    p.emit(as::cgti(25, 20, -1));      // pop not marked
    p.emit(as::nor(26, 20, 20));
    p.emit(as::selb(24, 26, 20, 25));  // pop, unmarked
    p.emit(as::sf(27, 24, 22));        // push - pop
    p.emit(as::clgt(28, 24, 22));      // pop > push: add 2 * depth
    p.emit(as::a(29, 23, 23));
    p.emit(as::and_(28, 28, 29));
    p.emit(as::a(27, 27, 28));         // entries
    p.emit(as::ceq(30, 27, 23));
    p.brnz(30, wait);                  // full
    p.br(claim);

    // Full: join the waiter list (count, then task ID; the same for workload IDs at 0x50), wait for a signal.
    p.bind(wait);
    p.emit(as::il(31, 0));
    p.emit(as::il(32, 1));
    p.emit(as::lqa(33, 0x27D0));
    p.emit(as::rotqbyi(33, 33, 4));    // task ID (0x27D4)
    p.emit(as::lqa(34, 0x1D0));
    p.emit(as::rotqbyi(34, 34, 12));   // workload ID (0x1DC)
    p.emit(as::lqa(35, 0xB0));         // queue + 0x30
    p.insert_byte(35, 32, 35, 0, 31);
    p.insert_byte(35, 33, 35, 1, 31);
    p.emit(as::stqa(35, 0xB0));
    p.emit(as::lqa(35, 0xD0));         // queue + 0x50
    p.insert_byte(35, 32, 35, 0, 31);
    p.insert_byte(35, 34, 35, 1, 31);
    p.emit(as::stqa(35, 0xD0));
    p.line(PS_MFC_PUTLLC, rQ, 11);
    p.brnz(11, step1);
    p.emit(as::lqa(37, 0x2FD0));       // the wait object (u64 at 0x2FD8): queue | 4
    p.emit(as::ori(38, rQ, 4));
    p.insert_word(37, 38, 37, 3, 31);
    p.insert_word(37, 31, 37, 2, 31);
    p.emit(as::stqa(37, 0x2FD0));
    p.syscall(2);
    p.emit(as::il(31, 0));
    p.emit(as::lqa(37, 0x2FD0));
    p.insert_word(37, 31, 37, 3, 31);
    p.emit(as::stqa(37, 0x2FD0));
    p.emit(as::ai(rWaits, rWaits, 1));
    p.emit(as::il(rWaited, 1));
    p.br(step1);

    // Claim: after a wait, leave the waiter list first (one waiter: clear the counts and the first slots).
    p.bind(claim);
    p.brz(rWaited, mark);
    p.emit(as::fsmbi(40, 0x3FFF));     // keeps bytes 2..15
    p.emit(as::lqa(41, 0xB0));
    p.emit(as::and_(41, 41, 40));
    p.emit(as::stqa(41, 0xB0));
    p.emit(as::lqa(41, 0xD0));
    p.emit(as::and_(41, 41, 40));
    p.emit(as::stqa(41, 0xD0));
    p.bind(mark);
    p.emit(as::il(31, 0));
    p.emit(as::nor(42, 22, 22));       // ~push
    p.insert_word(43, 42, 20, 1, 31);
    p.emit(as::stqa(43, 0x80));
    p.line(PS_MFC_PUTLLC, rQ, 11);
    p.brnz(11, step1);
    // The entry, by DMA to entries + (push mod depth) * size.
    p.emit(as::clgt(44, 23, 22));      // depth > push
    p.emit(as::sf(45, 23, 22));        // push - depth
    p.emit(as::selb(46, 45, 22, 44));  // slot
    p.emit(as::rotqbyi(47, 20, 8));    // size
    p.emit(as::mpyu(48, 46, 47));
    p.emit(as::rotqbyi(49, 21, 4));    // entries (low word)
    p.emit(as::a(49, 49, 48));
    p.emit(as::il(50, 0));
    p.insert_word(51, rV, 50, 0, 50);
    p.insert_word(51, rI, 51, 1, 50);
    p.emit(as::ila(52, kBuf));
    p.emit(as::stqd(51, 0, 52));
    p.dma(PS_MFC_PUT, 52, 49, 47);

    // Step 2: the push index advances, unmarked; wake the PPU if it waits on the empty queue.
    p.bind(step2);
    p.line(PS_MFC_GETLLAR, rQ, 11);
    p.emit(as::lqa(20, 0x80));
    p.emit(as::lqa(21, 0x90));
    p.emit(as::rotqbyi(22, 20, 4));
    p.emit(as::nor(22, 22, 22));       // push (it was ours, marked)
    p.emit(as::rotqbyi(23, 20, 12));
    p.emit(as::cgti(25, 20, -1));      // pop not marked
    p.emit(as::nor(26, 20, 20));
    p.emit(as::selb(24, 26, 20, 25));
    p.emit(as::sf(27, 24, 22));
    p.emit(as::clgt(28, 24, 22));
    p.emit(as::a(29, 23, 23));
    p.emit(as::and_(28, 28, 29));
    p.emit(as::a(27, 27, 28));         // entries before this one
    p.emit(as::ai(53, 22, 1));
    p.emit(as::ceq(54, 53, 29));
    p.emit(as::andc(53, 53, 54));      // push + 1, wrapping at 2 * depth
    p.emit(as::il(31, 0));
    p.insert_word(55, 53, 20, 1, 31);
    p.emit(as::stqa(55, 0x80));
    p.line(PS_MFC_PUTLLC, rQ, 11);
    p.brnz(11, step2);
    p.brnz(25, no_event);              // no PPU pop under way
    p.brnz(27, no_event);              // the queue was not empty
    p.emit(as::rotqbyi(56, 21, 8));    // SPU port
    p.emit(as::ceqi(57, 56, -1));
    p.brnz(57, no_event);
    p.emit(as::il(58, 0));             // sys_spu_thread_throw_event(port, 0, 0)
    p.emit(as::wrch(PS_SPU_WrOutMbox, 58));
    p.emit(as::ai(59, 56, 64));
    p.emit(as::shli(59, 59, 24));
    p.emit(as::wrch(PS_SPU_WrOutIntrMbox, 59));
    p.bind(no_event);
    p.emit(as::ai(rI, rI, 1));
    p.emit(as::ai(rV, rV, 1));
    p.emit(as::il(rWaited, 0));
    p.br(next);

    p.bind(done);
    p.emit(as::il(31, 0));
    p.insert_word(60, rI, 31, 0, 31);
    p.insert_word(60, rWaits, 60, 1, 31);
    p.emit(as::ila(52, kBuf));
    p.emit(as::stqd(60, 0, 52));
    p.emit(as::il(61, 16));
    p.dma(PS_MFC_PUT, 52, rReport, 61);
    p.syscall(0);
    p.finish();
    return p;
}

// A task ELF at `ea`: one read-only PT_LOAD at 0x3000 with the program (what libsre's loader accepts).
void write_task_elf(u8* base, u32 ea, const Program& p)
{
    const u32 size = static_cast<u32>(p.code.size() * 4);
    REQUIRE(0x3000 + size <= kBuf);
    std::memset(base + ea, 0, 0x100 + size + 16);
    static constexpr u8 kIdent[16] = {0x7F, 'E', 'L', 'F', 1, 2, 1, 0};
    std::memcpy(base + ea, kIdent, 16);
    ps_write16(base, ea + 0x10, 2);    // ET_EXEC
    ps_write16(base, ea + 0x12, 0x17); // EM_SPU
    ps_write32(base, ea + 0x14, 1);
    ps_write32(base, ea + 0x18, 0x3000); // entry
    ps_write32(base, ea + 0x1C, 0x34);   // phoff
    ps_write16(base, ea + 0x28, 0x34);
    ps_write16(base, ea + 0x2A, 0x20);
    ps_write16(base, ea + 0x2C, 1);
    const u32 ph = ea + 0x34;
    ps_write32(base, ph + 0x00, 1);      // PT_LOAD
    ps_write32(base, ph + 0x04, 0x100);  // offset
    ps_write32(base, ph + 0x08, 0x3000); // vaddr
    ps_write32(base, ph + 0x0C, 0x3000); // paddr
    ps_write32(base, ph + 0x10, size);
    ps_write32(base, ph + 0x14, (size + 15) & ~15u);
    ps_write32(base, ph + 0x18, 5); // R X
    ps_write32(base, ph + 0x1C, 0x80);
    for (usize i = 0; i < p.code.size(); i++) ps_write32(base, ea + 0x100 + 4 * static_cast<u32>(i), p.code[i]);
}

} // namespace

TEST_CASE("SPURS queues: an SPU task pushes and the PPU pops, each waking the other", "[hle][spurs][spurs-queue]")
{
    QueueFixture s(true); // the native scheduler runs the task
    u8* b = s.b;
    constexpr u32 kDepth = 2, kCount = 5, kFirst = 100;
    s.make_queue(kQueue, 16, kDepth);
    write_task_elf(b, kElf, pusher_task());
    ps_write32(b, kArg + 0, kQueue);
    ps_write32(b, kArg + 4, kCount);
    ps_write32(b, kArg + 8, kFirst);
    ps_write32(b, kArg + 12, kReport);
    std::memset(b + kPattern, 0, 16);
    ps_write8(b, kPattern + 15, 1); // the stack's block: the task can wait

    // The pops run on their own PPU thread; pop i starts once `allowed` > i.
    std::atomic<u32> allowed{1}, popped{0};
    std::vector<u64> rcs(kCount, ~0ull);
    const hle::Function& pop = GuestThread::find("cellSpursQueuePopBody");
    std::thread popper([&] {
        GuestThread p(s.f, 1, "popper");
        for (u32 i = 0; i < kCount; i++) {
            if (!wait_for([&] { return allowed.load() > i; }, std::chrono::milliseconds(20000))) break;
            rcs[i] = pop_on(p, pop, kQueue, kOut + 16 * i, 0, 1);
            popped = i + 1;
        }
    });
    // Pop 0 finds the queue empty: its index stays marked while it waits for the task's push and the event.
    REQUIRE(wait_for([&] { return line_word(b, kQueue) == ~0u; }));
    REQUIRE(s.a.call("cellSpursCreateTask", kTs, kId, kElf, kCtx, 0xC00, kPattern, kArg) == 0);
    const u32 id = ps_read32(b, kId);

    // The task waits to push: it is on the waiter list, the queue is full with no push under way, and the task is
    // switched out (waiting, not running, not ready, no signal yet).
    auto task_waits = [&] {
        alignas(16) u8 line[128];
        ps_res_acquire(b, kQueue, line);
        const u32 pop = ps_read32(line, 0), push = ps_read32(line, 4);
        return ps_read8(line, 0x30) == 1 && !marked(push) && entries(unmarked(pop), push, kDepth) == kDepth &&
               s.task_bit(0x50, id) && !s.task_bit(0x00, id) && !s.task_bit(0x10, id) && !s.task_bit(0x40, id);
    };
    // 100 woke pop 0; then the task filled the queue (101, 102) and waits with 103. Its saved context has the wait
    // object at +0x358 (LS 0x2FD8): the queue, type 4.
    REQUIRE(wait_for([&] { return popped.load() == 1 && task_waits(); }));
    CHECK(rcs[0] == 0);
    CHECK(ps_read64(b, kCtx + 0x358) == (u64{kQueue} | 4));
    CHECK(ps_read8(b, kQueue + 0x31) == id);
    CHECK(ps_read8(b, kQueue + 0x51) == s.wid());
    // Pop 1 takes from the full queue and signals the task, which pushes 103 and waits again with 104.
    allowed = 2;
    REQUIRE(wait_for([&] { return popped.load() == 2 && task_waits(); }));
    // Pop 2 wakes it for the last push; pops 3 and 4 find their entries (or wait for them).
    allowed = kCount;
    popper.join();
    for (u32 i = 0; i < kCount; i++) {
        CAPTURE(i);
        CHECK(rcs[i] == 0);
        CHECK(ps_read32(b, kOut + 16 * i) == kFirst + i);
        CHECK(ps_read32(b, kOut + 16 * i + 4) == i);
    }
    REQUIRE(wait_for([&] { return load32_acquire(b, kReport) == kCount; })); // the report's PUT (D-585)
    // The waits for 103 and 104; a third when the task found the queue full before pop 0 was done (pop 0 woke it).
    const u32 waits = ps_read32(b, kReport + 4);
    CHECK((waits == 2 || waits == 3));
    REQUIRE(wait_for([&] { return !s.task_bit(0x30, id); })); // exited
    CHECK(ps_read32(b, kQueue + 0x00) == kCount % (2 * kDepth));
    CHECK(ps_read32(b, kQueue + 0x04) == kCount % (2 * kDepth));
    CHECK(ps_read8(b, kQueue + 0x30) == 0);
    REQUIRE(s.a.call("cellSpursQueueDetachLv2EventQueue", kQueue) == 0);
}

TEST_CASE("SPURS queues: cellSpursQueuePushBody checks, entries in order, full, the indices' wrap; cellSpursQueueSize",
    "[hle][spurs][spurs-queue]")
{
    QueueFixture s;
    u8* b = s.b;
    constexpr u32 kSize = 32, kDepth = 4;
    REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, kQueue, kEntries, kSize, kDepth, PPU2SPU) == 0);
    REQUIRE(s.a.call("cellSpursQueueAttachLv2EventQueue", kQueue) == 0);
    auto push = [&](u32 value, u32 blocking) {
        put_entry(b, kOut, value, kSize);
        return s.a.call("cellSpursQueuePushBody", kQueue, kOut, blocking);
    };
    auto size = [&] {
        ps_write32(b, kNumber, 0xDEAD);
        REQUIRE(s.a.call("cellSpursQueueSize", kQueue, kNumber) == 0);
        return ps_read32(b, kNumber);
    };

    CHECK(s.a.call("cellSpursQueuePushBody", 0, kOut, 0) == kNull);
    CHECK(s.a.call("cellSpursQueuePushBody", kQueue, 0, 0) == kNull);
    CHECK(s.a.call("cellSpursQueuePushBody", kQueue + 0x40, kOut, 0) == kAlign);
    REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, kQueue2, kEntries + 0x800, kSize, kDepth, SPU2PPU) == 0);
    CHECK(s.a.call("cellSpursQueuePushBody", kQueue2, kOut, 0) == kPerm); // SPU tasks push to SPU2PPU queues
    REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, kQueue2, kEntries + 0x800, kSize, kDepth, SPU2SPU) == 0);
    CHECK(s.a.call("cellSpursQueuePushBody", kQueue2, kOut, 1) == kPerm);
    REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, kQueue2, kEntries + 0x800, kSize, kDepth, PPU2SPU) == 0);
    CHECK(s.a.call("cellSpursQueuePushBody", kQueue2, kOut, 1) == kStat); // blocking needs the lv2 queue
    CHECK(s.a.call("cellSpursQueuePushBody", kQueue2, 0x40000000, 0) == kFault); // refused before the claim (D-042)
    CHECK(ps_read32(b, kQueue2 + 0x04) == 0);
    put_entry(b, kOut, 11, kSize);
    REQUIRE(s.a.call("cellSpursQueuePushBody", kQueue2, kOut, 0) == 0); // TryPush doesn't need it
    REQUIRE(spu_pop(s, kQueue2, 11));
    CHECK(s.a.call("cellSpursQueueSize", 0, kNumber) == kNull);
    CHECK(s.a.call("cellSpursQueueSize", kQueue, 0) == kNull);
    CHECK(s.a.call("cellSpursQueueSize", kQueue + 0x40, kNumber) == kAlign);

    // Four entries fill the queue, in their slots; the fifth is AGAIN and leaves the line as it was.
    CHECK(size() == 0);
    for (u32 v = 1; v <= 4; v++) {
        REQUIRE(push(v, v & 1) == 0); // Push and TryPush take the same path while there is room
        CHECK(entry_is(b + kEntries + (v - 1) * kSize, v, kSize));
        CHECK(ps_read32(b, kQueue + 0x04) == v);
        CHECK(size() == v);
    }
    CHECK(push(5, 0) == kAgain);
    CHECK(push(5, 0x100) == kAgain); // only the low byte counts
    CHECK(ps_read32(b, kQueue + 0x04) == 4);

    // The SPU pops them in order; the indices run to 2 * depth and wrap, an entry's slot is its index mod depth.
    for (u32 v = 1; v <= 4; v++) REQUIRE(spu_pop(s, kQueue, v));
    CHECK_FALSE(spu_pop(s, kQueue, 0)); // empty
    for (u32 v = 5; v <= 8; v++) REQUIRE(push(v, 1) == 0);
    CHECK(ps_read32(b, kQueue + 0x04) == 0); // index 7 was the last before the wrap
    CHECK(entry_is(b + kEntries + 3 * kSize, 8, kSize));
    REQUIRE(spu_pop(s, kQueue, 5));
    REQUIRE(push(9, 0) == 0); // index 0, slot 0
    CHECK(entry_is(b + kEntries, 9, kSize));
    CHECK(size() == 4);

    // Size reads through the marks of calls under way.
    ps_write32(b, kQueue + 0x04, ~1u);
    CHECK(size() == 4); // pop 5, push 1 (marked): 1 - 5 + 2 * depth
    // Another PPU caller's push under way (its index marked): BUSY, blocking or not.
    CHECK(push(10, 1) == kBusy);
    CHECK(push(10, 0) == kBusy);
    ps_write32(b, kQueue + 0x04, 1);
    CHECK(s.a.call("cellSpursQueueDetachLv2EventQueue", kQueue) == 0);
}

TEST_CASE("SPURS queues: a blocking push waits for the SPU pop's event on the queue's SPU port",
    "[hle][spurs][spurs-queue]")
{
    QueueFixture s;
    u8* b = s.b;
    constexpr u32 kSize = 16, kDepth = 2;
    REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, kQueue, kEntries, kSize, kDepth, PPU2SPU) == 0);
    REQUIRE(s.a.call("cellSpursQueueAttachLv2EventQueue", kQueue) == 0);
    const u32 port = ps_read32(b, kQueue + 0x18);
    for (u32 v = 1; v <= 2; v++) {
        put_entry(b, kOut, v, kSize);
        REQUIRE(s.a.call("cellSpursQueuePushBody", kQueue, kOut, 0) == 0);
    }

    std::atomic<u64> rc{~0ull};
    const hle::Function& push = GuestThread::find("cellSpursQueuePushBody");
    put_entry(b, kOut + 0x100, 3, kSize);
    std::thread pusher([&] {
        GuestThread p(s.f, 1, "pusher");
        p.ctx.gpr[3] = kQueue;
        p.ctx.gpr[4] = kOut + 0x100;
        p.ctx.gpr[5] = 1;
        push.fn(p.ctx, p.f.base());
        rc = p.ctx.gpr[3];
    });
    // The PPU marks the push index for its whole call and waits on the lv2 queue.
    REQUIRE(wait_for([&] { return line_word(b, kQueue + 0x04) == ~2u; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(rc.load() == ~0ull);
    put_entry(b, kOut, 9, kSize);
    CHECK(s.a.call("cellSpursQueuePushBody", kQueue, kOut, 0) == kBusy); // one PPU caller at a time
    CHECK(s.a.call("cellSpursQueueDetachLv2EventQueue", kQueue) == kBusy);

    // An event while the queue is still full: the PPU looks again and goes on waiting.
    throw_event(s.f, kSpurs, port);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(rc.load() == ~0ull);

    // The SPU pop: step 2 sees the marked push index of a full queue and wakes the PPU through the port.
    SpuPop p{b, kQueue};
    REQUIRE(p.claim());
    CHECK(p.read_is(1));
    REQUIRE(p.finish() == port);
    throw_event(s.f, kSpurs, port);
    pusher.join();
    CHECK(rc.load() == 0);
    CHECK(entry_is(b + kEntries, 3, kSize)); // index 2, slot 0
    CHECK(ps_read32(b, kQueue + 0x04) == 3); // advanced, unmarked
    REQUIRE(spu_pop(s, kQueue, 2));
    REQUIRE(spu_pop(s, kQueue, 3));
    REQUIRE(s.a.call("cellSpursQueueDetachLv2EventQueue", kQueue) == 0);
}

TEST_CASE("SPURS queues: a push into an empty queue signals the oldest SPU task waiting to pop",
    "[hle][spurs][spurs-queue]")
{
    QueueFixture s; // no workers: the tasks' states are set here
    u8* b = s.b;
    constexpr u32 kSize = 16, kDepth = 2;
    REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, kQueue, kEntries, kSize, kDepth, PPU2SPU) == 0);
    const u8 wid = static_cast<u8>(s.wid());
    // Tasks 3 and 5 exist and wait; both are in the queue's pop waiter list, 3 first.
    ps_write32(b, kTs + 0x30, (0x80000000u >> 3) | (0x80000000u >> 5)); // enabled
    ps_write32(b, kTs + 0x50, (0x80000000u >> 3) | (0x80000000u >> 5)); // waiting
    add_pop_waiter(b, kQueue, 3, wid);
    add_pop_waiter(b, kQueue, 5, wid);

    // The push fills the empty queue: task 3 gets the signal, and its workload too.
    put_entry(b, kOut, 1, kSize);
    REQUIRE(s.a.call("cellSpursQueuePushBody", kQueue, kOut, 0) == 0);
    CHECK(s.task_bit(0x40, 3));
    CHECK_FALSE(s.task_bit(0x40, 5));
    CHECK(ps_read16(b, kSpurs + 0x70) & (0x8000u >> wid)); // wklSignal1
    // Not empty any more: the next push signals no one; the PPU never edits the list.
    put_entry(b, kOut, 2, kSize);
    REQUIRE(s.a.call("cellSpursQueuePushBody", kQueue, kOut, 0) == 0);
    CHECK_FALSE(s.task_bit(0x40, 5));
    CHECK(ps_read8(b, kQueue + 0x20) == 2);

    // A waiter's task ID libsre's _cellSpursSendSignal refuses with INVAL: the push is done, the result is FATAL.
    REQUIRE(s.a.call("_cellSpursQueueInitialize", 0, kTs, kQueue2, kEntries + 0x100, kSize, 1, PPU2SPU) == 0);
    add_pop_waiter(b, kQueue2, 200, wid);
    put_entry(b, kOut, 7, kSize);
    CHECK(s.a.call("cellSpursQueuePushBody", kQueue2, kOut, 0) == kFatal);
    CHECK(entry_is(b + kEntries + 0x100, 7, kSize));
    CHECK(ps_read32(b, kQueue2 + 0x04) == 1);
}
