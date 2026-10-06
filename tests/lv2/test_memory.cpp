// sys_memory and sys_mmapper through the dispatcher.

#include "lv2_test_util.hpp"
#include "pspocket/core/memory_map.h"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

namespace {

constexpr u64 k1M = 0x400, k64K = 0x200;
constexpr u64 kNoShmKey = 0xFFFF000000000000ull;

struct UserSize {
    u32 total, available;
};

UserSize user_size(Fixture& f, Caller& c)
{
    const u32 info = f.alloc(8);
    REQUIRE(c.call(kMemUserSize, info) == CELL_OK);
    return {f.r32(info), f.r32(info + 4)};
}

u32 allocate(Fixture& f, Caller& c, u64 size, u64 flags)
{
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kMemAllocate, size, flags, out) == CELL_OK);
    return f.r32(out);
}

} // namespace

TEST_CASE("lv2 memory: user memory allocation, page sizes and accounting", "[lv2][memory]")
{
    Fixture f;
    Caller c(*f.kernel);
    const UserSize before = user_size(f, c);
    CHECK(before.total == 0xD500000); // RPCS3's size for SDK > 2.2
    CHECK(before.available == before.total);

    const u32 a = allocate(f, c, 0x200000, 0);
    CHECK(a % 0x100000 == 0);
    CHECK(a >= 0x10000000);
    CHECK(a < PS_MEM_RSX_LOCAL_BASE);
    CHECK((a < PS_MEM_PRX_BASE || a >= PS_MEM_PRX_BASE + PS_MEM_PRX_SIZE));
    CHECK(f.mem.is_committed(a));
    CHECK(f.mem.is_committed(a + 0x1FFFFF));
    CHECK(f.r64(a + 0x1000) == 0);
    f.w32(a + 0x1FFFFC, 0xCAFEBABE);
    CHECK(f.r32(a + 0x1FFFFC) == 0xCAFEBABE);

    const u32 b = allocate(f, c, 0x30000, k64K);
    CHECK(b % 0x10000 == 0);
    CHECK((b >> 28) != (a >> 28)); // 64 KB and 1 MB pages come from different areas
    const u32 b2 = allocate(f, c, 0x10000, k64K);
    CHECK((b2 >> 28) == (b >> 28));
    CHECK(user_size(f, c).available == before.available - 0x200000 - 0x30000 - 0x10000);

    const u32 out = f.alloc(4);
    CHECK(c.call(kMemAllocate, 0, 0, out) == rc(CELL_EALIGN));
    CHECK(c.call(kMemAllocate, 0x10000, k1M, out) == rc(CELL_EALIGN));
    CHECK(c.call(kMemAllocate, 0x100000, 0x100, out) == rc(CELL_EINVAL));
    CHECK(c.call(kMemAllocate, 0x100000, 0, 0) == rc(CELL_EFAULT));
    CHECK(c.call(kMemAllocate, 0xE000000, 0, out) == rc(CELL_ENOMEM));
    CHECK(user_size(f, c).available == before.available - 0x240000); // failures give everything back

    CHECK(c.call(kMemFree, a + 0x10000) == rc(CELL_EINVAL));
    CHECK(c.call(kMemFree, a + 0x100) == rc(CELL_EINVAL));
    REQUIRE(c.call(kMemFree, a) == CELL_OK);
    CHECK_FALSE(f.mem.is_committed(a));
    CHECK(c.call(kMemFree, a) == rc(CELL_EINVAL));
    // Reallocated memory reads as zero again.
    const u32 a2 = allocate(f, c, 0x200000, k1M);
    CHECK(f.r32(a2 + 0x1FFFFC) == 0);
    REQUIRE(c.call(kMemFree, a2) == CELL_OK);
    REQUIRE(c.call(kMemFree, b) == CELL_OK);
    REQUIRE(c.call(kMemFree, b2) == CELL_OK);
    CHECK(user_size(f, c).available == before.available);
}

TEST_CASE("lv2 memory: allocations until exhaustion and back", "[lv2][memory]")
{
    Config config;
    config.user_memory_size = 0x2000000; // 32 MB keeps the test small
    Fixture f(config);
    Caller c(*f.kernel);
    std::vector<u32> blocks;
    const u32 out = f.alloc(4);
    for (;;) {
        const s32 r = c.call(kMemAllocate, 0x100000, 0, out);
        if (r != CELL_OK) {
            CHECK(r == rc(CELL_ENOMEM));
            break;
        }
        blocks.push_back(f.r32(out));
    }
    CHECK(blocks.size() == 32);
    CHECK(user_size(f, c).available == 0);
    for (u32 addr : blocks) REQUIRE(c.call(kMemFree, addr) == CELL_OK);
    CHECK(user_size(f, c).available == 0x2000000);
}

TEST_CASE("lv2 memory: containers", "[lv2][memory]")
{
    Fixture f;
    Caller c(*f.kernel);
    const UserSize before = user_size(f, c);
    const u32 out = f.alloc(4);
    CHECK(c.call(kContainerCreate, out, 0x80000) == rc(CELL_ENOMEM)); // rounds down to 0
    REQUIRE(c.call(kContainerCreate, out, 0x201234) == CELL_OK);
    const u32 cid = f.r32(out);
    CHECK((cid >> 24) == 0x3F);
    const u32 info = f.alloc(8);
    REQUIRE(c.call(kContainerGetSize, info, cid) == CELL_OK);
    CHECK(f.r32(info) == 0x200000);
    CHECK(f.r32(info + 4) == 0x200000);
    CHECK(user_size(f, c).total == before.total - 0x200000);

    REQUIRE(c.call(kMemAllocateFromContainer, 0x100000, cid, 0, out) == CELL_OK);
    const u32 a = f.r32(out);
    REQUIRE(c.call(kContainerGetSize, info, cid) == CELL_OK);
    CHECK(f.r32(info + 4) == 0x100000);
    CHECK(c.call(kMemAllocateFromContainer, 0x200000, cid, 0, out) == rc(CELL_ENOMEM));
    CHECK(c.call(kMemAllocateFromContainer, 0x100000, 0x3F00000F, 0, out) == rc(CELL_ESRCH));
    CHECK(c.call(kContainerDestroy, cid) == rc(CELL_EBUSY));
    REQUIRE(c.call(kMemFree, a) == CELL_OK);
    REQUIRE(c.call(kContainerDestroy, cid) == CELL_OK);
    CHECK(c.call(kContainerGetSize, info, cid) == rc(CELL_ESRCH));
    CHECK(user_size(f, c).total == before.total);
    CHECK(user_size(f, c).available == before.available);
}

TEST_CASE("lv2 memory: page attributes", "[lv2][memory]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 attr = f.alloc(24);
    const u32 big = allocate(f, c, 0x100000, k1M);
    const u32 small = allocate(f, c, 0x10000, k64K);
    REQUIRE(c.call(kMemPageAttribute, big + 0x1234, attr) == CELL_OK);
    CHECK(f.r64(attr) == 0x40000); // read/write
    CHECK(f.r32(attr + 16) == 0x100000);
    REQUIRE(c.call(kMemPageAttribute, small, attr) == CELL_OK);
    CHECK(f.r32(attr + 16) == 0x10000);
    CHECK(c.call(kMemPageAttribute, 0x70000000, attr) == rc(CELL_EINVAL));
    CHECK(c.call(kMemPageAttribute, big, 0) == rc(CELL_EFAULT));
}

TEST_CASE("lv2 mmapper: shared memory objects map, alias and persist", "[lv2][mmapper]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 out = f.alloc(4);
    CHECK(c.call(kMmAllocateAddress, 0x08000000, k1M, 0, out) == rc(CELL_EALIGN));
    CHECK(c.call(kMmAllocateAddress, 0x10000000, k1M, 0x1000, out) == rc(CELL_EALIGN));
    REQUIRE(c.call(kMmAllocateAddress, 0x10000000, k1M, 0x10000000, out) == CELL_OK);
    const u32 area = f.r32(out);
    CHECK(area % 0x10000000 == 0);
    CHECK(area >= 0x10000000);
    CHECK(area < PS_MEM_RSX_LOCAL_BASE);

    CHECK(c.call(kMmAllocateShm, kNoShmKey, 0, k1M, out) == rc(CELL_EALIGN));
    CHECK(c.call(kMmAllocateShm, kNoShmKey, 0x10000, k1M, out) == rc(CELL_EALIGN));
    CHECK(c.call(kMmAllocateShm, kNoShmKey, 0x100000, 0x300, out) == rc(CELL_EINVAL));
    const u32 avail = user_size(f, c).available;
    REQUIRE(c.call(kMmAllocateShm, kNoShmKey, 0x100000, k1M, out) == CELL_OK);
    const u32 mem = f.r32(out);
    CHECK((mem >> 24) == 0x08);
    CHECK(user_size(f, c).available == avail - 0x100000);

    // Two mappings of one object alias the same pages.
    REQUIRE(c.call(kMmMap, area, mem, 0) == CELL_OK);
    CHECK(c.call(kMmMap, area, mem, 0) == rc(CELL_EBUSY));
    CHECK(c.call(kMmMap, area + 0x1000, mem, 0) == rc(CELL_EALIGN));
    CHECK(c.call(kMmMap, 0x70000000, mem, 0) == rc(CELL_EINVAL)); // not an mmapper area
    REQUIRE(c.call(kMmMap, area + 0x100000, mem, 0) == CELL_OK);
    f.w64(area + 0x40, 0x1122334455667788ull);
    CHECK(f.r64(area + 0x100000 + 0x40) == 0x1122334455667788ull);
    f.w32(area + 0x100000 + 0xFFFFC, 0xABCD0123);
    CHECK(f.r32(area + 0xFFFFC) == 0xABCD0123);

    const u32 got = f.alloc(4);
    REQUIRE(c.call(kMmUnmap, area + 0x100000, got) == CELL_OK);
    CHECK(f.r32(got) == mem);
    CHECK_FALSE(f.mem.is_committed(area + 0x100000));
    CHECK(c.call(kMmUnmap, area + 0x100000, got) == rc(CELL_EINVAL));
    CHECK(c.call(kMmFreeShm, mem) == rc(CELL_EBUSY)); // still mapped once
    REQUIRE(c.call(kMmUnmap, area, got) == CELL_OK);

    // The object keeps its contents while unmapped.
    REQUIRE(c.call(kMmSearchAndMap, area, mem, 0, out) == CELL_OK);
    const u32 found = f.r32(out);
    CHECK(found >= area);
    CHECK(found % 0x100000 == 0);
    CHECK(f.r64(found + 0x40) == 0x1122334455667788ull);
    CHECK(c.call(kMmSearchAndMap, area + 0x100000, mem, 0, out) == rc(CELL_EINVAL)); // not the area start

    CHECK(c.call(kMmFreeAddress, area) == rc(CELL_EBUSY));
    REQUIRE(c.call(kMmUnmap, found, got) == CELL_OK);
    REQUIRE(c.call(kMmFreeShm, mem) == CELL_OK);
    CHECK(c.call(kMmFreeShm, mem) == rc(CELL_ESRCH));
    CHECK(user_size(f, c).available == avail);
    REQUIRE(c.call(kMmFreeAddress, area) == CELL_OK);
    CHECK(c.call(kMmFreeAddress, area) == rc(CELL_EINVAL));

    // 64 KB-granular objects can't go into a 1 MB-page area.
    REQUIRE(c.call(kMmAllocateAddress, 0x10000000, k1M, 0, out) == CELL_OK);
    const u32 area2 = f.r32(out);
    REQUIRE(c.call(kMmAllocateShm, kNoShmKey, 0x10000, k64K, out) == CELL_OK);
    const u32 small = f.r32(out);
    CHECK(c.call(kMmMap, area2, small, 0) == rc(CELL_EINVAL));
    CHECK(c.call(kMmSearchAndMap, area2, small, 0, out) == rc(CELL_EALIGN));
    REQUIRE(c.call(kMmFreeShm, small) == CELL_OK);
    REQUIRE(c.call(kMmFreeAddress, area2) == CELL_OK);
}

TEST_CASE("lv2 mmapper: IPC keys", "[lv2][mmapper]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 out = f.alloc(4);
    const u64 key = 0x1234;
    const u64 create_new = k1M | 0xC000, create_any = k1M | 0x8000, open_only = k1M;
    CHECK(c.call(kMmAllocateShm, key, 0x100000, open_only, out) == rc(CELL_ESRCH));
    REQUIRE(c.call(kMmAllocateShm, key, 0x100000, create_new, out) == CELL_OK);
    const u32 mem = f.r32(out);
    CHECK(c.call(kMmAllocateShm, key, 0x100000, create_new, out) == rc(CELL_EEXIST));
    CHECK(c.call(kMmAllocateShm, key, 0x100000, create_any, out) == rc(CELL_EEXIST));
    CHECK(c.call(kMmAllocateShm, key, 0x200000, create_any, out) == rc(CELL_EPERM)); // different geometry
    REQUIRE(c.call(kMmFreeShm, mem) == CELL_OK);
    REQUIRE(c.call(kMmAllocateShm, key, 0x100000, create_any, out) == CELL_OK);
    REQUIRE(c.call(kMmFreeShm, f.r32(out)) == CELL_OK);
}

TEST_CASE("lv2 sys_vm: map the whole virtual size, charge the physical size, paging calls validate ranges", "[lv2][vm]")
{
    // GTA V's first sys_vm call (docs/runs/006): 32 MB virtual, 0x1260000 physical, default container, 64 KB pages.
    constexpr u32 kVmMap = 300, kVmUnmap = 301, kVmAppend = 302, kVmReturn = 303, kVmLock = 304, kVmTouch = 306,
                  kVmStore = 309, kVmTest = 311, kVmStats = 312;
    constexpr u64 kPolicy = 1;
    const Result kInval = err(CELL_EINVAL), kBusy = err(CELL_EBUSY);
    Fixture f;
    Caller c(*f.kernel);
    const UserSize before = user_size(f, c);
    const u32 out = f.alloc(8);

    CHECK(c.call(kVmMap, 0x2000000, 0x1260000, 0xFFFFFFFF, k64K, 2, out) == kInval);  // policy
    CHECK(c.call(kVmMap, 0x1000000, 0x1260000, 0xFFFFFFFF, k64K, kPolicy, out) == kInval); // vsize not 32 MB-aligned
    CHECK(c.call(kVmMap, 0x2000000, 0x80000, 0xFFFFFFFF, k64K, kPolicy, out) == kInval);  // psize below 1 MB
    REQUIRE(c.call(kVmMap, 0x2000000, 0x1260000, 0xFFFFFFFF, k64K, kPolicy, out) == CELL_OK);
    const u32 vm = f.r32(out);
    CHECK(vm % 0x10000000 == 0);
    CHECK(vm >= 0x10000000);
    CHECK(f.mem.is_committed(vm));
    CHECK(f.mem.is_committed(vm + 0x1FFFFFF)); // the whole virtual size, not just the physical part
    CHECK_FALSE(f.mem.is_committed(vm + 0x2000000));
    f.w32(vm + 0x1FFFFFC, 0x12345678);
    CHECK(f.r32(vm + 0x1FFFFFC) == 0x12345678);
    CHECK(user_size(f, c).available == before.available - 0x1260000);

    CHECK(c.call(kVmLock, vm, 0x1000) == CELL_OK);
    CHECK(c.call(kVmTouch, vm + 0x1FFF000, 0x1000) == CELL_OK);
    CHECK(c.call(kVmStore, vm + 0x1FFF000, 0x2000) == kInval); // past the end
    CHECK(c.call(kVmLock, vm, 0) == kInval);
    const u32 state = f.alloc(8);
    REQUIRE(c.call(kVmTest, vm, 0x10000, state) == CELL_OK);
    CHECK(f.r64(state) == 2); // SYS_VM_STATE_ON_MEMORY

    REQUIRE(c.call(kVmAppend, vm, 0x100000) == CELL_OK);
    CHECK(c.call(kVmAppend, vm, 0x10000) == kInval); // 1 MB granularity
    CHECK(c.call(kVmReturn, vm, 0x1300000) == kBusy); // would leave less than 1 MB
    REQUIRE(c.call(kVmReturn, vm, 0x200000) == CELL_OK);
    const u32 stats = f.alloc(0x30);
    REQUIRE(c.call(kVmStats, vm, stats) == CELL_OK);
    CHECK(f.r32(stats + 0x20) == 0x1260000 + 0x100000 - 0x200000);
    CHECK(user_size(f, c).available == before.available - 0x1160000);

    // At most 256 MB of virtual memory per process.
    const u32 out2 = f.alloc(4);
    for (int i = 0; i < 7; i++) REQUIRE(c.call(kVmMap, 0x2000000, 0x100000, 0xFFFFFFFF, k64K, kPolicy, out2) == CELL_OK);
    CHECK(c.call(kVmMap, 0x2000000, 0x100000, 0xFFFFFFFF, k64K, kPolicy, out2) == kBusy);

    CHECK(c.call(kVmUnmap, vm + 0x1000) == kInval);
    REQUIRE(c.call(kVmUnmap, vm) == CELL_OK);
    CHECK_FALSE(f.mem.is_committed(vm));
    CHECK(c.call(kVmUnmap, vm) == kInval);
    CHECK(c.call(kVmMap, 0x2000000, 0x100000, 0xFFFFFFFF, k64K, kPolicy, out2) == CELL_OK); // room again
}

TEST_CASE("lv2 mmapper: page fault notification, registered with RPCS3's checks (D-184)", "[lv2][memory]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kMmAllocateAddress, 0x10000000, k1M, 0x10000000, out) == CELL_OK);
    const u32 area = f.r32(out);
    REQUIRE(c.call(kQueueCreate, out, queue_attr(f, SYS_SYNC_FIFO), 0, 8) == CELL_OK);
    const u32 queue = f.r32(out);
    CHECK(c.call(327, area + 0x10000, queue) == rc(CELL_EINVAL)); // not the area's start
    CHECK(c.call(327, 0x00100000, queue) == rc(CELL_EINVAL));     // not an mmapper area
    CHECK(c.call(327, area, 0x8D00FFFF) == rc(CELL_ESRCH));       // no such queue
    REQUIRE(c.call(327, area, queue) == CELL_OK);
    CHECK(c.call(327, area, queue) == rc(CELL_EBUSY)); // once per area
    const u32 n = f.alloc(4);
    REQUIRE(c.call(kGetNumberOfObject, 0x0E, n) == CELL_OK);
    CHECK(f.r32(n) == 1); // the port lv2 made for it
}
