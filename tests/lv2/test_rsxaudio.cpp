// sys_rsxaudio (D-527 part 4): the context, its shared page, the client's queues, and the hardware retiring ring
// buffer entries once per block period with events at each notify step.

#include <chrono>
#include <thread>

#include "lv2_test_util.hpp"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

namespace {

constexpr u32 kInit = 650, kFinalize = 651, kImport = 652, kUnimport = 653, kConnect = 654, kClose = 655,
              kPrepare = 656, kStart = 657, kStop = 658, kDmaParam = 659;
constexpr u32 kCtrl = 0x1C000, kRingSize = 560;

u32 make_queue(Fixture& f, Caller& c, s32 size)
{
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kQueueCreate, out, queue_attr(f, SYS_SYNC_FIFO), 0, size) == CELL_OK);
    return f.r32(out);
}

} // namespace

TEST_CASE("lv2 RSX audio: a context, its connection and the hardware's schedule", "[lv2][rsxaudio]")
{
    Fixture f;
    Caller c(*f.kernel, "audio");
    const u32 out = f.alloc(8);
    CHECK(c.call(kInit, 0) == rc(CELL_EFAULT));
    REQUIRE(c.call(kInit, out) == CELL_OK);
    const u32 h = f.r32(out);
    CHECK((h & 0xFF000000) == 0x60000000);
    CHECK(c.call(kInit, out) == rc(CELL_EINVAL)); // one context at a time
    REQUIRE(c.call(kImport, h, out) == CELL_OK);
    const u32 page = static_cast<u32>(f.r64(out));
    CHECK(f.mem.is_committed(page + 0x1FFFF));
    CHECK(f.r32(page + kCtrl + 1740) == 0x8000);
    CHECK(f.r32(page + kCtrl + 1744) == 0xDEADBEEF);
    CHECK(c.call(kImport, 0x60FFFF00, out) == rc(CELL_ESRCH));
    REQUIRE(c.call(kDmaParam, h, 0, out) == CELL_OK);
    CHECK(f.r64(out) == 0x30000000);
    REQUIRE(c.call(kDmaParam, h, 1, out) == CELL_OK);
    CHECK(f.r64(out) == 1);

    // The client's queues: all three must exist.
    const u32 q1 = make_queue(f, c, 64), q2 = make_queue(f, c, 8), q3 = make_queue(f, c, 8);
    f.w32(page + kCtrl + 1684, q1);
    f.w32(page + kCtrl + 1704, 0x8d00ff00);
    CHECK(c.call(kConnect, h) == rc(CELL_ESRCH));
    f.w32(page + kCtrl + 1704, q2);
    f.w32(page + kCtrl + 1724, q3);
    REQUIRE(c.call(kConnect, h) == CELL_OK);
    const u32 rb = page + kCtrl;
    CHECK(f.r32(rb + 32) == 0x30000000 + 0x18000);              // the silence block
    CHECK(f.r32(rb + 48 + 32 * 3 + 28) == 0x30000000 + 0x3000); // serial entry 3's DMA address
    CHECK(f.r32(rb + kRingSize + 48 + 32 * 2 + 28) == 0x30000000 + 0x10000 + 0x800);

    // The serial ring buffer: 4 entries in use, a notification every second entry (RPCS3's rule: with more than two
    // entries in use, the notify index must come back to 1); 3 of them filled.
    f.w32(rb + 0, 1);  // active
    f.w32(rb + 16, 4); // rw_max_idx
    f.w32(rb + 24, 2); // queue_notify_step
    REQUIRE(c.call(kPrepare, h) == CELL_OK);
    CHECK(c.call(kPrepare, h) == -1); // already the hardware's
    REQUIRE(c.call(kStart, h) == CELL_OK);
    for (u32 i = 0; i < 3; i++) {
        f.w32(rb + 48 + 32 * i, 1);           // valid
        f.w64(rb + 48 + 32 * i + 8, 100 + i); // audio_blk_idx
    }
    const u32 events = f.alloc(32 * 16), number = f.alloc(4);
    u32 got = 0;
    std::vector<u64> blocks;
    for (int tries = 0; tries < 200 && got < 3; tries++) { // the start event, then entries 0 and 2 retired
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(c.call(kQueueTryReceive, q1, events, 16, number) == CELL_OK);
        for (u32 i = 0; i < f.r32(number); i++) {
            CHECK(f.r64(events + 32 * i) == 0x8000000000400100);
            CHECK(f.r64(events + 32 * i + 8) == 0);
            blocks.push_back(f.r64(events + 32 * i + 16));
            got++;
        }
    }
    REQUIRE(got == 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(blocks[0] == 0); // the start event
    CHECK(f.r32(rb + 8) == 3); // read_idx: three retired
    CHECK(f.r32(rb + 48) == 0); // entry 0 no longer valid
    REQUIRE(c.call(kStop, h) == CELL_OK);
    CHECK(f.r32(rb + 8) == 0); // cleared
    CHECK(c.call(kClose, h) == CELL_OK);
    CHECK(c.call(kUnimport, h, out) == CELL_OK);
    REQUIRE(c.call(kFinalize, h) == CELL_OK);
    CHECK(c.call(kFinalize, h) == rc(CELL_ESRCH));
    CHECK(!f.mem.is_committed(page));
    REQUIRE(c.call(kInit, out) == CELL_OK); // a new context
}
