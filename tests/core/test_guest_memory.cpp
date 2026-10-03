#include <catch_amalgamated.hpp>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/memory_map.h"

using namespace pspocket;

TEST_CASE("guest memory reserves 4 GB and commits on demand")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.base() != nullptr);
    CHECK(mem.committed_bytes() == 0);
    CHECK_FALSE(mem.is_committed(PS_MEM_MAIN_BASE));

    REQUIRE(mem.commit(PS_MEM_MAIN_BASE, 0x100000));
    CHECK(mem.is_committed(PS_MEM_MAIN_BASE));
    CHECK(mem.is_committed(PS_MEM_MAIN_BASE + 0xFFFFF));
    CHECK(mem.committed_bytes() >= 0x100000);

    // Fresh pages read as zero.
    CHECK(mem.read<u64>(PS_MEM_MAIN_BASE) == 0);

    mem.write<u32>(PS_MEM_MAIN_BASE + 0x10, 0xDEADBEEF);
    CHECK(mem.read<u32>(PS_MEM_MAIN_BASE + 0x10) == 0xDEADBEEF);
    CHECK(mem.read<u8>(PS_MEM_MAIN_BASE + 0x10) == 0xDE); // big-endian in guest memory
    mem.write<double>(PS_MEM_MAIN_BASE + 0x20, 3.5);
    CHECK(mem.read<double>(PS_MEM_MAIN_BASE + 0x20) == 3.5);
}

TEST_CASE("guest memory covers the top of the address space")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(PS_MEM_RSX_LOCAL_BASE, PS_MEM_RSX_LOCAL_SIZE));
    mem.write<u32>(PS_MEM_RSX_LOCAL_BASE + PS_MEM_RSX_LOCAL_SIZE - 4, 0x12345678);
    CHECK(mem.read<u32>(PS_MEM_RSX_LOCAL_BASE + PS_MEM_RSX_LOCAL_SIZE - 4) == 0x12345678);

    const u32 last_page = static_cast<u32>(GuestMemory::kGuestSize - GuestMemory::host_page_size());
    REQUIRE(mem.commit(last_page, GuestMemory::host_page_size()));
    mem.write<u32>(0xFFFFFFFC, 0xCAFEF00D);
    CHECK(mem.read<u32>(0xFFFFFFFC) == 0xCAFEF00D);
}

TEST_CASE("decommit returns zeroed pages")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(PS_MEM_STACK_BASE, 0x10000));
    mem.write<u32>(PS_MEM_STACK_BASE, 0x11223344);
    REQUIRE(mem.decommit(PS_MEM_STACK_BASE, 0x10000));
    CHECK_FALSE(mem.is_committed(PS_MEM_STACK_BASE));
    REQUIRE(mem.commit(PS_MEM_STACK_BASE, 0x10000));
    CHECK(mem.read<u32>(PS_MEM_STACK_BASE) == 0);
}

TEST_CASE("ranges outside the guest space are rejected")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    CHECK_FALSE(mem.commit(0xFFFF0000, 0x20000));
    CHECK_FALSE(mem.commit(0x1000, 0));
}
