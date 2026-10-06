// PRX placement as data (D-178): the firmware's fixed layout, and a module relocated where it is placed, as the loader
// places it (tests/hle/test_lle.cpp checks the same relocations through the loader) and the system library's
// preparation translates it.

#include <catch_amalgamated.hpp>

#include "pspocket/core/be.h"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/firmware.hpp"
#include "pspocket/recomp/prx_place.hpp"
#include "synthetic.hpp"

using namespace pspocket;
using namespace pspocket::recomp;

TEST_CASE("PRX placement: the fixed layout, and a module relocated at its fixed addresses (D-178)")
{
    const auto x = test::build_ppu_prx();
    auto elf = ElfFile::parse(x.bytes);
    REQUIRE(elf);
    const std::vector<u32> sizes = prx_load_sizes(*elf);
    REQUIRE(sizes == std::vector<u32>{0x700, 0x300});

    // Modules one after another from the base, each PT_LOAD at a 64 KB boundary; an empty one gets 0.
    const std::vector<std::vector<u32>> all = {sizes, {0x10, 0}, sizes};
    const auto layout = prx_fixed_layout(all, 0x30000000, 0x10000000);
    REQUIRE(layout.size() == 3);
    CHECK(layout[0] == std::vector<u32>{0x30000000, 0x30010000});
    CHECK(layout[1] == std::vector<u32>{0x30020000, 0});
    CHECK(layout[2] == std::vector<u32>{0x30030000, 0x30040000});
    CHECK(prx_fixed_layout(all, 0x30000000, 0x40000).empty()); // they do not fit

    auto placed = place_prx(*elf, x.bytes, layout[2]);
    REQUIRE(placed);
    REQUIRE(placed->segments.size() == 2);
    CHECK(placed->relocations == x.relocations - 2);
    CHECK(placed->errors.size() == 2); // the unknown type and the segment that does not exist
    const u32 seg0 = 0x30030000, seg1 = 0x30040000;
    const u8* s0 = placed->segments[0].bytes.data();
    const u8* s1 = placed->segments[1].bytes.data();
    CHECK(placed->segments[0].executable);
    CHECK_FALSE(placed->segments[1].executable);
    CHECK(ps_ld32(s0 + 0x6F0) == 0x12345678u);       // ADDR32, absolute
    CHECK(ps_ld64(s0 + 0x690) == seg1 + 0x10);       // ADDR64 into segment 1
    const u32 slot0 = seg1 + 0x80;                   // lis/lwz of the first import stub reach its slot
    CHECK(ps_ld16(s0 + 0x102) == static_cast<u16>((slot0 >> 16) + ((slot0 & 0x8000) ? 1 : 0)));
    CHECK(ps_ld16(s0 + 0x106) == static_cast<u16>(slot0));
    CHECK(ps_ld32(s1 + 0x00) == seg0);               // module_start's descriptor: its code, its TOC
    CHECK(ps_ld32(s1 + 0x04) == seg1 + 0x8100);

    // Placed elsewhere, the code differs: what the loader checks a module against before running its translation.
    auto elsewhere = place_prx(*elf, x.bytes, layout[0]);
    REQUIRE(elsewhere);
    CHECK(elsewhere->segments[0].bytes != placed->segments[0].bytes);
    CHECK_FALSE(place_prx(*elf, x.bytes, std::vector<u32>{0x30000000}));     // an address per PT_LOAD
    CHECK_FALSE(place_prx(*elf, x.bytes, std::vector<u32>{0, 0x30010000}));   // the first one has none
}

TEST_CASE("firmware modules: which pspocket serves itself, which are translated, the identity line (D-178)")
{
    CHECK(firmware_module_native("libsre.sprx"));
    CHECK(firmware_module_native("libsysutil_savedata.sprx"));
    CHECK(firmware_module_native("internal/sys_audio.sprx"));
    CHECK(firmware_module_native("internal/libfs_utility_init.sprx"));
    CHECK_FALSE(firmware_module_native("libpngdec.sprx"));
    CHECK_FALSE(firmware_module_native("internal/libtrhddec.sprx"));
    CHECK(firmware_module_translated("libfont.sprx"));
    CHECK(firmware_module_translated("internal/libm2aacdec.sprx"));
    CHECK_FALSE(firmware_module_translated("libgcm_sys.sprx"));
    CHECK_FALSE(firmware_module_translated("internal/sys_init_osd.self"));
    CHECK(firmware_identity_line("libfont.sprx", "ab12", std::vector<u32>{0x30010000, 0}) == "libfont.sprx ab12 30010000 0\n");
}
