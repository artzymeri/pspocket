#pragma once

// The system library (D-178): the PS3 firmware's own modules (the user's /dev_flash, D-156), translated once per
// firmware and shared by every game, so the modules a game gets from the firmware (D-175) run as native code instead
// of under the interpreter. Every module has a fixed address in the PRX region (LleModules, recomp::prx_fixed_layout),
// the same for every game, so the code is translated from the modules as the loader places them; the ones pspocket
// always serves itself are left out (recomp::firmware_module_translated). Made by prepare_game's compile, link and
// sign steps, into its own folder: game.dylib, manifest.json, and
//   identity.txt  what it was made from (LleModules::identity_text); the library holds its SHA-256
//   modules.txt   "M key address size sha256" per code segment: the loader runs a module's translation only when the
//                 module in guest memory is the one translated

#include <string>

#include "pspocket/prepare/prepare.hpp"

namespace pspocket::prepare {

struct SystemOptions {
    std::string dev_flash; // the installed /dev_flash
    std::string dir;       // the system library's folder
    // As for a game (Options): the compiler, workers, memory, signing, progress, cancel, sources kept, rates.
    Toolchain toolchain;
    unsigned workers = 0;
    double memory_mb = 0;
    const codesign::SigningKey* key = nullptr;
    std::string identifier;
    double prior_rates[3] = {1.0, 1.0, 1.0};
    int thermal = 0;
    bool keep_sources = false;
    std::function<void(const Progress&)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

inline constexpr const char* kSystemTitle = "PS3-SYSTEM";

// The translation step on its own: the modules placed, analysed and lifted into dir/ppu, identity.txt and modules.txt
// written. Returns the number of modules translated (0 with *error on failure).
size_t translate_system(const std::string& dev_flash, const std::string& dir, std::string* error,
    std::string* summary = nullptr);

// translate_system, then compile, link and sign (prepare_game with dir/ppu and identity.txt).
Result prepare_system(const SystemOptions& options);

// Whether dir holds a system library this build can load for the firmware whose identity text is `identity`.
bool system_library_is_current(const std::string& dir, const std::string& identity, std::string* why);

} // namespace pspocket::prepare
