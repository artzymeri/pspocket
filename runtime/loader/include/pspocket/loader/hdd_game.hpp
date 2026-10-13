// Games that start from /dev_hdd0/game (D-939), as RPCS3 starts them (Emulator::Load). An installed PSN game (its
// PARAM.SFO's category HG, installed from its package: ps3disc --install-pkg, the app) runs from its folder, with the
// folder's USRDIR as /app_home. A disc game whose update is installed (the folder named by the disc's title ID, with a
// PARAM.SFO of that title ID and category GD and an executable of its own, USRDIR/EBOOT.BIN) runs the update's
// executable, its disc in the drive. Either way the executable's guest path (argv[0]) is in /dev_hdd0/game/<folder>,
// and the folder has to be in the one mounted as /dev_hdd0 (pspocket mounts top-level names only).
#ifndef PSPOCKET_LOADER_HDD_GAME_HPP
#define PSPOCKET_LOADER_HDD_GAME_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/hle/process.hpp"

namespace pspocket::lv2 {
class Kernel;
}

namespace pspocket::loader {

struct HddGame {
    std::string dir;      // the folder's name under /dev_hdd0/game ("NPEB00001")
    std::string host_dir; // the folder on the host
    std::string title_id; // PARAM.SFO's TITLE_ID, else the folder's name
    std::string title;    // PARAM.SFO's TITLE
    std::string category; // "HG" an installed game, "GD" game data (an update when it has an executable)
    std::string app_ver;  // PARAM.SFO's APP_VER ("01.05"), else empty
    std::vector<u8> param_sfo;
    std::string boot_path; // the executable's guest path, argv[0]: /dev_hdd0/game/<dir>/USRDIR/EBOOT.BIN

    bool update() const { return category == "GD"; }
    // As cellGame sees it: started from /dev_hdd0/game/<dir>, with the folder's PARAM.SFO.
    hle::GameInfo game_info() const;
};

// The installed game in the folder game_dir, which has to be in hdd0_dir/game (hdd0_dir is the folder that is
// /dev_hdd0), its PARAM.SFO read. Nullopt with *error when the folder is elsewhere or has no PARAM.SFO that parses.
std::optional<HddGame> read_hdd_game(const std::string& hdd0_dir, const std::string& game_dir, std::string* error);

// The update installed for the disc game title_id: hdd0_dir/game/<title_id> with a PARAM.SFO of that title ID and
// category GD and its own USRDIR/EBOOT.BIN, as RPCS3 looks for it; else nullopt.
std::optional<HddGame> find_update(const std::string& hdd0_dir, std::string_view title_id);

// The installed game whose folder (hdd0_dir/game/<dir>) holds the executable, with boot_path the executable's own
// guest path; nullopt when the executable is not in hdd0_dir/game. A folder there without a PARAM.SFO that parses is
// nullopt too, with *error set.
std::optional<HddGame> hdd_game_of(const std::string& hdd0_dir, const std::string& executable, std::string* error);

// Mounts the folder's USRDIR as /app_home (writable, as RPCS3 mounts the executable's folder), in place of a disc's.
bool mount_app_home(lv2::Kernel& kernel, const HddGame& game, std::string* error);

} // namespace pspocket::loader

#endif // PSPOCKET_LOADER_HDD_GAME_HPP
