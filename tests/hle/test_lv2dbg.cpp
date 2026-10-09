// sys_lv2dbg against liblv2dbg_for_cex.sprx (D-193's method, D-521): every function with made-up arguments, the
// answers and the bytes written the same. Skipped without the firmware (PSPOCKET_TEST_DEV_FLASH); no game data.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

TEST_CASE("sys_lv2dbg against liblv2dbg_for_cex", "[hle][lle][lv2dbg]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    Fixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "sys_lv2dbg");
    u8* b = f.base();
    constexpr u32 kOut = kData + 0x1000;
    std::vector<std::string> names;
    for (const Function& fn : registry().functions())
        if (fn.module == "sys_lv2dbg") names.emplace_back(fn.name);
    REQUIRE(names.size() == 35);
    for (const std::string& name : names) {
        for (const u64 n : {u64{0}, u64{1}, u64{5}}) {
            const std::vector<u64> args = {0x10000, n, kOut, kOut + 0x100, 0x1234};
            std::memset(b + kOut, 0xA5, 0x200);
            const Function& fn = GuestThread::find(name);
            for (usize i = 0; i < args.size(); i++) t.ctx.gpr[3 + i] = args[i];
            fn.fn(t.ctx, b);
            const u32 ours = static_cast<u32>(t.ctx.gpr[3]);
            const std::vector<u8> ours_out(b + kOut, b + kOut + 0x200);
            std::memset(b + kOut, 0xA5, 0x200);
            const u32 sonys = sony.call(t, recomp::nid_of(name), {args[0], args[1], args[2], args[3], args[4]});
            INFO(std::format("{}({}): ours {:#x}, Sony's {:#x}", name, n, ours, sonys));
            CHECK(ours == sonys);
            CHECK(std::memcmp(ours_out.data(), b + kOut, 0x200) == 0);
        }
    }
}
