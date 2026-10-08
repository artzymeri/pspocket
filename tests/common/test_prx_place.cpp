// PRX placement as data (D-178): the firmware's fixed layout, and a module relocated where it is placed, as the loader
// places it (tests/hle/test_lle.cpp checks the same relocations through the loader) and the system library's
// preparation translates it.

#include <catch_amalgamated.hpp>

#include "pspocket/core/be.h"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/firmware.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/recomp/prx_place.hpp"
#include "pspocket/recomp/self_decrypt.hpp"
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

TEST_CASE("game modules: an identity that follows the module, not the file around it (D-238)")
{
    const auto x = test::build_ppu_prx();
    auto elf = ElfFile::parse(x.bytes);
    REQUIRE(elf);
    const std::string id = prx_identity(*elf);
    CHECK(id.size() == 64);
    // Read by offset from an archive: the bytes after it are not the module's.
    std::vector<u8> archived = x.bytes;
    archived.insert(archived.end(), 4096, 0xAB);
    auto in_archive = ElfFile::parse(archived);
    REQUIRE(in_archive);
    CHECK(prx_identity(*in_archive) == id);
    // The same module from its debug SELF.
    const std::vector<u8> self = test::wrap_in_self(x.bytes, {.debug = true, .npdrm = false, .self_type = 4});
    auto decrypted = decrypt_self(self, {});
    REQUIRE(decrypted);
    auto from_self = ElfFile::parse(*decrypted);
    REQUIRE(from_self);
    CHECK(prx_identity(*from_self) == id);
    // One code byte changed: another module.
    std::vector<u8> changed = x.bytes;
    changed[elf->segments()[0].offset + 0x40] ^= 1;
    auto other = ElfFile::parse(changed);
    REQUIRE(other);
    CHECK(prx_identity(*other) != id);
}

TEST_CASE("game modules: slots from the top of the PRX region down, never moved, never below the floor (D-238)")
{
    constexpr u32 kTop = PS_MEM_PRX_BASE + PS_MEM_PRX_SIZE, kFloor = 0x3F000000;
    const std::vector<std::vector<u32>> first = {{0x700, 0x300}, {0x20000, 0, 0x100}};
    const auto a = game_module_layout(first, kTop, kFloor);
    REQUIRE(a.size() == 2);
    CHECK(a[0] == std::vector<u32>{0x3FFE0000, 0x3FFF0000}); // two 64 KB blocks under the top
    CHECK(a[1] == std::vector<u32>{0x3FFB0000, 0, 0x3FFD0000});
    CHECK(game_module_layout(first, kTop, kFloor) == a); // the same every time

    // The next preparation appends below the lowest slot: the earlier ones keep theirs.
    GameModuleLayout layout{"ab", kFloor, {{std::string(64, '1'), a[0], "/dev_bdvd/PS3_GAME/USRDIR/a.sprx"},
                                              {std::string(64, '2'), a[1], "/dev_bdvd/PS3_GAME/USRDIR/my mod.sprx"}}};
    CHECK(layout.lowest() == 0x3FFB0000);
    // A module too big for what is left gets nothing; the one after it still gets a slot.
    const std::vector<std::vector<u32>> more = {{0x2000000}, {0x100}};
    const auto b = game_module_layout(more, layout.lowest(), kFloor);
    REQUIRE(b.size() == 2);
    CHECK(b[0].empty());
    CHECK(b[1] == std::vector<u32>{0x3FFA0000});
    for (const auto& m : {a[0], a[1], b[1]})
        for (const u32 addr : m) CHECK((addr == 0 || (addr >= kFloor && addr < kTop && (addr & 0xFFFF) == 0)));

    // As text and back.
    const std::string text = game_module_layout_text(layout);
    auto parsed = parse_game_module_layout(text);
    REQUIRE(parsed);
    CHECK(parsed->executable_sha256 == "ab");
    CHECK(parsed->floor == kFloor);
    REQUIRE(parsed->modules.size() == 2);
    CHECK(parsed->modules[1].addrs == a[1]);
    CHECK(parsed->modules[1].source == "/dev_bdvd/PS3_GAME/USRDIR/my mod.sprx");
    REQUIRE(parsed->find(std::string(64, '2')));
    CHECK(parsed->find(std::string(64, '3')) == nullptr);
    CHECK_FALSE(parse_game_module_layout("X 1\n"));
    CHECK_FALSE(parse_game_module_layout("G short 1 30000000 a\n"));
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
