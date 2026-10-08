#pragma once

// pspocket's own libraries against Sony's (D-193, "measure each against Sony's translated version first"): the
// firmware module of a library, from the user's own installed system software, placed and started in a test fixture
// as the loader runs it (LLE, D-157), so the same calls can go to both and their answers be compared. The firmware is
// read where the user installed it and never enters the repository (CLAUDE.md rule 1): PSPOCKET_TEST_DEV_FLASH names
// the /dev_flash (scripts/gate.sh sets it when inventory/firmware/installed/dev_flash exists); without it the
// comparisons are skipped.

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "guest.hpp" // runtime/hle/src: call_guest
#include "hle_test_util.hpp"
#include "pspocket/loader/lle.hpp"

namespace pspocket::hle::test {

// The installed /dev_flash to compare with, or none.
inline std::optional<std::string> test_dev_flash()
{
    const char* v = std::getenv("PSPOCKET_TEST_DEV_FLASH");
    if (!v || !*v) return std::nullopt;
    return std::string(v);
}

// The firmware module exporting `library`, loaded into `f` and started on `t`. Its functions are called by NID.
// `start` false leaves module_start out: liblv2's is the process's own start-up (it loads the process's modules
// through lv2), which a test's functions do not need.
struct FirmwareLibrary {
    std::string library;
    loader::LleModules lle;
    const loader::PrxModule* module = nullptr;

    FirmwareLibrary(Fixture& f, GuestThread& t, const std::string& dev_flash, std::string lib, bool start = true)
        : library(std::move(lib)), lle(dev_flash)
    {
        REQUIRE(lle.index());
        lle.force(library); // its module, whatever the host has
        std::string why;
        module = lle.load(library, *f.hle, &why);
        INFO(why);
        REQUIRE(module);
        for (const u32 s : lle.take_starts())
            if (start) call_guest(t.ctx, s, {0, 0});
    }

    // Calls the module's export `nid` with up to eight arguments: r3's low word.
    u32 call(GuestThread& t, u32 nid, std::initializer_list<u64> args) const
    {
        const auto it = module->functions.find({library, nid});
        REQUIRE(it != module->functions.end());
        return static_cast<u32>(call_guest(t.ctx, it->second, args));
    }
};

} // namespace pspocket::hle::test
