#include <catch_amalgamated.hpp>

#include "pspocket/core/immutable_memory.hpp"

using namespace pspocket;

TEST_CASE("read-only guest ranges: inside one range only, cleared per executable (D-1013)", "[core]")
{
    clear_immutable_ranges();
    CHECK_FALSE(immutable_range(0x10000, 4));
    add_immutable_range(0x10000, 0x20000);
    add_immutable_range(0x30000, 0x40000);
    CHECK(immutable_range(0x10000, 0x10000));
    CHECK(immutable_range(0x1F000, 0x1000));
    CHECK(immutable_range(0x30010, 0x100));
    CHECK_FALSE(immutable_range(0x1F000, 0x1001)); // past its end
    CHECK_FALSE(immutable_range(0xF000, 0x2000));  // before its start
    CHECK_FALSE(immutable_range(0x1F000, 0x12000)); // across two ranges and the gap between
    CHECK_FALSE(immutable_range(0xFFFFFFF0u, 0x20)); // wraps
    add_immutable_range(0x50000, 0x50000); // empty: not marked
    CHECK_FALSE(immutable_range(0x50000, 0));
    for (u32 i = 0; i < 10; i++) add_immutable_range(0x100000 * (i + 1), 0x100000 * (i + 1) + 0x10);
    CHECK(immutable_range(0x100000, 0x10)); // the third range of eight
    CHECK_FALSE(immutable_range(0x800000, 0x10)); // the tenth was over the limit: checked as ever
    clear_immutable_ranges();
    CHECK_FALSE(immutable_range(0x10000, 4));
}
