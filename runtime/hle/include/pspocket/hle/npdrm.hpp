#pragma once

// The licence keys a running game has shown the system (D-581), as RPCS3's loaded_npdrm_keys (rpcs3/Crypto/unedat.h):
// sceNpDrmIsAvailable (and its exitspawn forms) installs the key that opens the content it checked (the klicensee
// for free content and SELFs, the RIF key of the player's RAP for network and local EDATs); lv2's file system tries
// them, newest first, on an EDATA file the game opens (RPCS3 sys_fs.cpp lv2_file::open_raw), and the PRX loader and
// SPU image loader take the newest for an NPDRM module (RPCS3 sys_prx.cpp, sys_spu.cpp: last_key()). Sixteen are
// kept, the oldest given up first, as RPCS3's ring.

#include <array>
#include <mutex>
#include <optional>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/npdrm.hpp"

namespace pspocket::lv2 {
class Kernel;
}

namespace pspocket::hle {

class NpdrmKeyRing {
public:
    static constexpr usize kSize = 16;

    void install(const recomp::Key128& key);
    // At most kSize keys, the last installed first.
    std::vector<recomp::Key128> newest_first() const;
    std::optional<recomp::Key128> newest() const;

private:
    mutable std::mutex mutex_;
    std::array<recomp::Key128, kSize> keys_{};
    u64 installed_ = 0;
};

// The current HLE process's ring (HleProcess::current()); nullptr outside one.
NpdrmKeyRing* npdrm_key_ring();

// The licence the running game's content opens with: `klicensee` for free content (none: NP_KLIC_FREE), and the
// player's RAPs read from /dev_hdd0/home/00000001/exdata through the kernel's file system.
recomp::NpdrmLicence process_licence(const lv2::Kernel& kernel, std::optional<recomp::Key128> klicensee);

// The licence an NPDRM module the game loads opens with (a PRX, an SPU image, an overlay): the newest key the game
// installed as its klicensee, as RPCS3's last_key(); NP_KLIC_FREE when it installed none (where RPCS3's zero key
// opens nothing).
recomp::NpdrmLicence module_licence(const lv2::Kernel& kernel);

} // namespace pspocket::hle
