// Dispatch table API: registration, lookup, growth, unregistration, the miss stub; .opd seeds.

#include <catch_amalgamated.hpp>

#include "pspocket/ppu/prx_dispatch.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/code_image.hpp"

using namespace pspocket;

namespace {
int g_called = 0;
void fn_a(PPUContext*, uint8_t*) { g_called = 1; }
void fn_b(PPUContext*, uint8_t*) { g_called = 2; }
} // namespace

TEST_CASE("dispatch table: register, find, grow in both directions, unregister, clear")
{
    ps_ppu_dispatch_clear();
    CHECK(ps_ppu_dispatch_count() == 0);
    CHECK(ps_ppu_dispatch_find(0x10000) == nullptr);
    CHECK(ps_ppu_dispatch_slot(0x10000) == ps_ppu_dispatch_miss_stub);

    ps_ppu_dispatch_register(0x00200000, fn_a);
    CHECK(ps_ppu_dispatch_find(0x00200000) == fn_a);
    CHECK(ps_ppu_dispatch_find(0x00200004) == nullptr);
    CHECK(ps_ppu_dispatch_slot(0x00200004) == ps_ppu_dispatch_miss_stub);

    // Growth downwards and upwards keeps existing entries.
    ps_ppu_dispatch_register(0x00010000, fn_b);
    ps_ppu_dispatch_register(0x00A00000, fn_b);
    CHECK(ps_ppu_dispatch_find(0x00200000) == fn_a);
    CHECK(ps_ppu_dispatch_find(0x00010000) == fn_b);
    CHECK(ps_ppu_dispatch_find(0x00A00000) == fn_b);
    CHECK(ps_ppu_dispatch_count() == 3);
    // Out of range and misaligned addresses miss.
    CHECK(ps_ppu_dispatch_slot(0x00000000) == ps_ppu_dispatch_miss_stub);
    CHECK(ps_ppu_dispatch_slot(0xFFFFFFFC) == ps_ppu_dispatch_miss_stub);
    CHECK(ps_ppu_dispatch_find(0x00200002) == nullptr);

    // Re-registering replaces without double counting.
    ps_ppu_dispatch_register(0x00200000, fn_b);
    CHECK(ps_ppu_dispatch_count() == 3);
    CHECK(ps_ppu_dispatch_find(0x00200000) == fn_b);

    ps_ppu_dispatch_unregister(0x00200000);
    CHECK(ps_ppu_dispatch_find(0x00200000) == nullptr);
    CHECK(ps_ppu_dispatch_count() == 2);
    ps_ppu_dispatch_unregister(0x00200000); // no-op
    CHECK(ps_ppu_dispatch_count() == 2);

    const ps_ppu_dispatch_entry table[] = {{0x00300000, fn_a}, {0x00300010, fn_b}};
    ps_ppu_dispatch_register_table(table, 2);
    PPUContext ctx{};
    ps_ppu_call_indirect(&ctx, nullptr, 0x00300010);
    CHECK(g_called == 2);
    CHECK(ctx.cia == 0x00300010);
    ps_ppu_call_indirect(&ctx, nullptr, 0x00300000);
    CHECK(g_called == 1);

    ps_ppu_dispatch_clear();
    CHECK(ps_ppu_dispatch_count() == 0);
    CHECK(ps_ppu_dispatch_find(0x00300000) == nullptr);
}

TEST_CASE("dispatch table: the PRX region's functions go to a second table and run once enabled (D-178)")
{
    ps_ppu_dispatch_clear();
    // A firmware module's function far above the game's code: the first table keeps to the game's range.
    const ps_ppu_dispatch_entry table[] = {{0x00010000, fn_a}, {0x30010000, fn_b}, {0x30020000, fn_a}};
    ps_ppu_dispatch_register_table(table, 3);
    CHECK(ps_ppu_dispatch_count() == 1);
    CHECK(u64{ps_ppu_dispatch.base} + 4ull * ps_ppu_dispatch.count < 0x30000000);
    CHECK(ppu::prx_dispatch_registered() == 2);
    CHECK(ppu::in_prx_region(0x30010000));
    CHECK_FALSE(ppu::in_prx_region(0x00010000));
    // Not before its module was checked.
    CHECK(ps_ppu_dispatch_find(0x30010000) == nullptr);
    ppu::prx_dispatch_enable(0x30010000, 0x30018000);
    CHECK(ps_ppu_dispatch_find(0x30010000) == fn_b);
    CHECK(ps_ppu_dispatch_find(0x30020000) == nullptr); // another module, not enabled
    CHECK(ps_ppu_dispatch_find(0x30010004) == nullptr);
    // An indirect call reaches it through the miss stub, which counts no miss.
    const u64 misses = ps_ppu_dispatch_miss_count();
    PPUContext ctx{};
    g_called = 0;
    ps_ppu_call_indirect(&ctx, nullptr, 0x30010000);
    CHECK(g_called == 2);
    CHECK(ps_ppu_dispatch_miss_count() == misses);
    // A later registration of one address wins; one registered alone joins the table.
    ps_ppu_dispatch_register(0x30010000, fn_a);
    CHECK(ps_ppu_dispatch_find(0x30010000) == fn_a);
    CHECK(ppu::prx_dispatch_registered() == 2);
    ps_ppu_dispatch_unregister(0x30010000);
    CHECK(ps_ppu_dispatch_find(0x30010000) == nullptr);
    ps_ppu_dispatch_clear();
    CHECK(ppu::prx_dispatch_registered() == 0);
    CHECK(ppu::prx_dispatch_enabled_ranges() == 0);
}

TEST_CASE("miss policy is configurable; default follows NDEBUG")
{
#ifdef NDEBUG
    CHECK(ps_ppu_get_miss_policy() == PS_PPU_MISS_INTERPRET);
#else
    CHECK(ps_ppu_get_miss_policy() == PS_PPU_MISS_ABORT);
#endif
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    ps_ppu_set_miss_policy(PS_PPU_MISS_ABORT);
    CHECK(ps_ppu_get_miss_policy() == PS_PPU_MISS_ABORT);
    ps_ppu_set_miss_policy(saved);
}

TEST_CASE(".opd descriptors become seeds with their TOC")
{
    using namespace pspocket::recomp::ppu;
    // code at 0x10000 (4 words), .opd at 0x10010: {0x10000, 0x8000}, {0x10008, 0x8000}, {0x99999990, 0x8000} (not code)
    const u32 words[] = {0x60000000, 0x4E800020, 0x60000000, 0x4E800020, 0x00010000, 0x00008000, 0x00010008, 0x00008000,
        0x99999990, 0x00008000};
    std::vector<u8> bytes;
    for (u32 w : words)
        for (int s = 24; s >= 0; s -= 8) bytes.push_back(static_cast<u8>(w >> s));
    const CodeImage image(0x10000, bytes);
    const std::vector<Seed> seeds = seeds_from_opd(image, 0x10010, 24);
    REQUIRE(seeds.size() == 2);
    CHECK(seeds[0].addr == 0x10000);
    CHECK(seeds[0].toc == 0x8000u);
    CHECK(seeds[1].addr == 0x10008);

    const auto s = parse_seed("0x1234:0x8000");
    REQUIRE(s);
    CHECK(s->addr == 0x1234);
    CHECK(s->toc == 0x8000u);
    CHECK_FALSE(parse_seed("zz").has_value());
}
