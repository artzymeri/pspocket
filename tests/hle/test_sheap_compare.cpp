// cellSheap and cellKeySheap against libsre.sprx (D-193's method, D-507): the same sequences of calls go to pspocket's
// heap and to Sony's, each on the same memory reset in between; every answer and every byte of the heap, its key table
// and the handles must be the same. Skipped without the firmware (PSPOCKET_TEST_DEV_FLASH); no game data.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kHeap = kData + 0x20000, kHeapSize = 0x10000; // the heap's memory (header, table, bitmap, blocks)
constexpr u32 kHandles = kData + 0x40000;                    // handles, 0x20 each
constexpr u8 kFill = 0x5A;

struct Op {
    const char* fn;
    std::vector<u64> args;
};

std::vector<u32> run(GuestThread& t, const std::vector<Op>& ops, const FirmwareLibrary* sony)
{
    u8* b = t.f.base();
    std::memset(b + kHeap, kFill, kHeapSize);
    std::memset(b + kHandles, kFill, 0x400);
    std::vector<u32> answers;
    for (const Op& op : ops) {
        u32 r3 = 0;
        if (sony) {
            const auto it = sony->module->functions.find({"cellSheap", recomp::nid_of(op.fn)});
            REQUIRE(it != sony->module->functions.end());
            std::array<u64, 8> a{};
            std::copy(op.args.begin(), op.args.end(), a.begin());
            r3 = static_cast<u32>(call_guest(t.ctx, it->second, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}));
        } else {
            const Function& fn = GuestThread::find(op.fn);
            for (usize i = 0; i < op.args.size(); i++) t.ctx.gpr[3 + i] = op.args[i];
            fn.fn(t.ctx, b);
            r3 = static_cast<u32>(t.ctx.gpr[3]);
        }
        answers.push_back(r3);
    }
    return answers;
}

void compare(GuestThread& t, const FirmwareLibrary& sony, const std::vector<Op>& ops, const std::string& what)
{
    u8* b = t.f.base();
    const std::vector<u32> mine = run(t, ops, nullptr);
    const std::vector<u8> mine_heap(b + kHeap, b + kHeap + kHeapSize), mine_handles(b + kHandles, b + kHandles + 0x400);
    const std::vector<u32> sonys = run(t, ops, &sony);
    for (usize i = 0; i < ops.size(); i++) {
        INFO(std::format("{}: step {} {}", what, i, ops[i].fn));
        CHECK(mine[i] == sonys[i]);
    }
    usize at = 0;
    while (at < kHeapSize && mine_heap[at] == b[kHeap + at]) at++;
    INFO(std::format("{}: heap differs first at +{:#x}", what, at));
    CHECK(at == kHeapSize);
    CHECK(std::memcmp(mine_handles.data(), b + kHandles, 0x400) == 0);
}

u32 handle(u32 i) { return kHandles + 0x20 * i; }

} // namespace

TEST_CASE("cellSheap and cellKeySheap against libsre", "[hle][lle][spurs][sheap]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    Fixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "cellSheap", false);

    // A plain heap: allocations of every size class, frees that merge, the queries after each.
    for (const u64 size : {u64{0x800}, u64{0x1000}, u64{0x4A80}, u64{kHeapSize}}) {
        std::vector<Op> ops = {{"cellSheapInitialize", {kHeap, size, 3}}, {"cellSheapQueryFree", {kHeap}},
            {"cellSheapQueryMax", {kHeap}}};
        const u64 sizes[] = {1, 0x80, 0x81, 0x100, 0x300, 0x80, 0x1000, 0x40, 0x200};
        for (u32 i = 0; i < std::size(sizes); i++) {
            ops.push_back({"cellSheapAllocate", {kHeap, sizes[i]}});
            ops.push_back({"cellSheapQueryFree", {kHeap}});
            ops.push_back({"cellSheapQueryMax", {kHeap}});
        }
        compare(t, sony, ops, std::format("heap of {:#x}", size));
    }
    {
        // Frees need the addresses Allocate gave: ours and Sony's are the same when the heaps agree, so the script takes
        // them from a run of ours.
        std::vector<Op> ops = {{"cellSheapInitialize", {kHeap, 0x2000, 0}}};
        for (const u64 s : {0x80, 0x100, 0x80, 0x200, 0x80}) ops.push_back({"cellSheapAllocate", {kHeap, s}});
        const std::vector<u32> addrs = run(t, ops, nullptr);
        for (const u32 i : {2u, 1u, 4u, 3u, 5u}) {
            ops.push_back({"cellSheapFree", {kHeap, addrs[i]}});
            ops.push_back({"cellSheapQueryFree", {kHeap}});
            ops.push_back({"cellSheapQueryMax", {kHeap}});
        }
        compare(t, sony, ops, "frees");
    }
    compare(t, sony, {{"cellSheapQueryFree", {kHeap + 8}}, {"cellSheapQueryMax", {kHeap + 0x40}}}, "misaligned queries");

    // A key sheap: initialize (and its refusals), every kind of object, a key taken twice, deletes in another order.
    const std::vector<Op> refusals = {{"cellKeySheapInitialize", {0, 0x4000, 0}},
        {"cellKeySheapInitialize", {kHeap + 0x40, 0x4000, 0}}, {"cellKeySheapInitialize", {kHeap, 0x7FF, 0}},
        {"cellKeySheapInitialize", {kHeap, 0x4000, 32}}, {"cellKeySheapInitialize", {kHeap, 0x8000000001ull, 0}}};
    compare(t, sony, refusals, "key sheap refusals");
    std::vector<Op> ops = {{"cellKeySheapInitialize", {kHeap, 0x6000, 5}}, {"cellSheapQueryFree", {kHeap}},
        {"cellSheapQueryMax", {kHeap}},
        {"cellKeySheapBufferNew", {handle(0), kHeap, 3, 0x200}},
        {"cellKeySheapBufferNew", {handle(1), kHeap, 3, 0x200}}, // the same key: shared
        {"cellKeySheapMutexNew", {handle(2), kHeap, 5}},
        {"cellKeySheapSemaphoreNew", {handle(3), kHeap, 6, 7}},
        {"cellKeySheapBarrierNew", {handle(4), kHeap, 7, 4}},
        {"cellKeySheapRwmNew", {handle(5), kHeap, 8, 0x80}},
        {"cellKeySheapQueueNew", {handle(6), kHeap, 9, 16, 4}},
        {"cellKeySheapQueueNew", {handle(7), kHeap, 10, 0x1000, 8}}, // too large: SHORTAGE
        {"cellKeySheapBarrierNew", {handle(8), kHeap, 11, 0}},      // cellSyncBarrierInitialize refuses 0
        {"cellKeySheapMutexNew", {handle(9), kHeap, 256}},
        {"cellKeySheapMutexNew", {0, kHeap, 1}},
        {"cellKeySheapMutexNew", {handle(9), 0, 1}},
        {"cellKeySheapBufferNew", {handle(9), kHeap + 8, 1, 0x80}},
        {"cellSheapQueryFree", {kHeap}}, {"cellSheapQueryMax", {kHeap}},
        {"cellKeySheapBufferDelete", {handle(0)}},
        {"cellKeySheapQueueDelete", {handle(6)}},
        {"cellKeySheapBufferDelete", {handle(1)}},
        {"cellKeySheapMutexDelete", {handle(2)}},
        {"cellKeySheapSemaphoreDelete", {handle(3)}},
        {"cellKeySheapRwmDelete", {handle(5)}},
        {"cellKeySheapBarrierDelete", {handle(4)}},
        {"cellSheapQueryFree", {kHeap}}, {"cellSheapQueryMax", {kHeap}},
        {"cellKeySheapMutexNew", {handle(2), kHeap, 5}}, // the key again, after it was let go
    };
    compare(t, sony, ops, "key sheap");
    // The objects were made (both sides agree, and agree on success).
    const std::vector<u32> answers = run(t, ops, nullptr);
    CHECK(answers[0] == 0);
    for (u32 i = 3; i <= 9; i++) CHECK(answers[i] == 0);
    CHECK(answers[10] == 0x80410312); // SHORTAGE
    CHECK(ps_read64(f.base(), handle(0) + 0x10) == ps_read64(f.base(), handle(1) + 0x10)); // shared
    CHECK(ps_read64(f.base(), handle(0) + 0x10) != 0);
}
