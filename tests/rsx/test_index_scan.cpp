// The indexed draw's index scan (D-957): the NEON scan against the scalar one, over random index buffers with restart
// indices, indices of all ones and bases that wrap past 20 bits.

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/rsx/index_scan.hpp"

using namespace pspocket;
using namespace pspocket::rsx;

namespace {

void check_same(const std::vector<u8>& bytes, u32 count, u32 size, u32 base, bool restart_on, u32 restart_index)
{
    IndexScan fast{}, slow{};
    std::vector<u32> fast_restarts, slow_restarts;
    scan_indices_be(bytes.data(), count, size, base, restart_on, restart_index, fast, fast_restarts);
    scan_indices_be_scalar(bytes.data(), count, size, base, restart_on, restart_index, slow, slow_restarts);
    INFO("count " << count << " size " << size << " base " << base << " restart " << restart_on << " " << restart_index);
    CHECK(fast.min_vi == slow.min_vi);
    CHECK(fast.max_vi == slow.max_vi);
    CHECK(fast.has_all_ones == slow.has_all_ones);
    CHECK(fast_restarts == slow_restarts);
}

} // namespace

TEST_CASE("index scan: the vector scan finds what the scalar one does (D-957)")
{
    std::mt19937 rng(957);
    for (u32 round = 0; round < 4000; round++) {
        const u32 size = (rng() & 1) ? 2 : 4;
        const u32 count = rng() % 200;
        const u32 base = (rng() & 3) == 0 ? 0xffff0 + rng() % 64 : rng() % 0x20000;
        const bool restart_on = rng() & 1;
        const u32 restart_index = size == 2 ? ((rng() & 1) ? 0xffffu : rng() % 64) : ((rng() & 1) ? 0xffffffffu : rng() % 64);
        std::vector<u8> bytes(count * size + 16);
        for (u32 i = 0; i < count; i++) {
            u32 v = rng() % 100;
            switch (rng() % 8) {
            case 0: v = restart_index; break;
            case 1: v = size == 2 ? 0xffffu : 0xffffffffu; break;
            case 2: v = rng(); break;
            default: break;
            }
            if (size == 2) ps_st16(bytes.data() + i * 2, static_cast<u16>(v));
            else ps_st32(bytes.data() + i * 4, v);
        }
        check_same(bytes, count, size, base, restart_on, restart_index);
    }
}

TEST_CASE("index scan: restart indices only, none, and widening a range already seen (D-957)")
{
    std::vector<u8> bytes(4 * 40);
    for (u32 i = 0; i < 40; i++) ps_st16(bytes.data() + i * 2, 0xffff);
    IndexScan scan{};
    std::vector<u32> restarts;
    scan_indices_be(bytes.data(), 40, 2, 7, true, 0xffff, scan, restarts);
    CHECK(scan.min_vi == ~0u);
    CHECK(scan.max_vi == 0);
    CHECK(scan.has_all_ones);
    CHECK(restarts.size() == 40);

    // Not a restart index: all ones reaches vertex (0xffff + base) & 0xfffff.
    scan = {};
    restarts.clear();
    scan_indices_be(bytes.data(), 40, 2, 7, false, 0xffff, scan, restarts);
    CHECK(scan.min_vi == 0x10006);
    CHECK(scan.max_vi == 0x10006);
    CHECK(restarts.empty());

    // The range widens what an earlier range found.
    for (u32 i = 0; i < 40; i++) ps_st32(bytes.data() + i * 4, 100 + i);
    scan = {50, 60, false};
    scan_indices_be(bytes.data(), 40, 4, 0, true, 0xffffffffu, scan, restarts);
    CHECK(scan.min_vi == 50);
    CHECK(scan.max_vi == 139);
    CHECK(!scan.has_all_ones);
}

// The scan's speed (hidden: `test_rsx "[.bench]"`): 3,000 16-bit indices, a mesh's worth, without restarts.
TEST_CASE("bench: index scan, 3,000 16-bit indices", "[.bench]")
{
    std::mt19937 rng(1);
    std::vector<u8> bytes(3000 * 2);
    for (u32 i = 0; i < 3000; i++) ps_st16(bytes.data() + i * 2, static_cast<u16>(rng() % 2000));
    std::vector<u32> restarts;
    for (const bool vector : {false, true}) {
        constexpr u32 kRounds = 20000;
        u32 sink = 0;
        const auto start = std::chrono::steady_clock::now();
        for (u32 r = 0; r < kRounds; r++) {
            IndexScan scan{};
            restarts.clear();
            if (vector) scan_indices_be(bytes.data(), 3000, 2, r & 7, true, 0xffff, scan, restarts);
            else scan_indices_be_scalar(bytes.data(), 3000, 2, r & 7, true, 0xffff, scan, restarts);
            sink += scan.max_vi;
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kRounds;
        std::printf("index scan (%s): %.0f ns for 3,000 indices (%u)\n", vector ? "NEON" : "scalar", ns, sink);
    }
}
