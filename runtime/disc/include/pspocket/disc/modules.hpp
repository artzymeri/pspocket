#pragma once

// A game's own executable files on its disc, found before the game runs (D-239): PS3_GAME/USRDIR's PPU PRX modules,
// decrypted with the build's keys and saved as the loader saves the modules it meets (<identity>.elf beside
// <identity>.txt, RuntimeModules::set_capture_dir), so the preparation translates them before the game ever loads
// them (prepare::prepare_game_modules); and its standalone SPU ELFs (thread images sys_spu_image_open reads), saved
// as found for the preparation's SPU scan.

#include <string>

#include "pspocket/core/types.hpp"

namespace pspocket::disc {

struct DiscModules {
    size_t prx = 0;        // PPU PRX modules saved (or there already)
    size_t spu = 0;        // SPU ELFs saved (or there already)
    size_t npdrm = 0;      // modules that need a PSN game's licence (not decrypted)
    size_t unreadable = 0; // executable-named files that are neither
    size_t files = 0;      // files looked at
};

// Walks PS3_GAME/USRDIR of the image (files up to 256 MB named as executables: .sprx .prx .self .elf .bin, read only
// when their first bytes say SELF or ELF) and saves into prx_dir and spu_dir. False with *error when the image cannot
// be read.
bool save_disc_modules(const std::string& image_path, const std::string& prx_dir, const std::string& spu_dir,
    DiscModules* out, std::string* error);

// The same for a game installed in /dev_hdd0/game (D-939): its folder's USRDIR walked on the host, a PSN module
// decrypted with its licence (free content, or the player's .rap under hdd0_dir, D-580), saved under its guest path
// /dev_hdd0/game/<folder>/USRDIR/... False with *error when the folder has no USRDIR.
bool save_installed_modules(const std::string& game_dir, const std::string& hdd0_dir, const std::string& prx_dir,
    const std::string& spu_dir, DiscModules* out, std::string* error);

} // namespace pspocket::disc
