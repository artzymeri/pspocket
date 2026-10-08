#pragma once

// A game's own PRX modules, translated (D-238): the modules a game loads from its disc while it runs (sys_prx_load_
// module), saved by the loader the first time (RuntimeModules::set_capture_dir: <identity>.elf), are given fixed slots
// at the top of the PRX region above the firmware's layout (recomp::game_module_layout, never moved once given),
// placed there, translated as the system library's modules are, and made into the game's "modules library" by
// prepare_game's compile, link and sign steps, in its own folder (game_dir/modules): game.dylib, manifest.json, and
//   identity.txt  the layout (recomp::game_module_layout_text: the executable, the floor, each module's identity, slot
//                 and source); the library holds its SHA-256
//   modules.txt   "M identity address size sha256" per code segment as placed, before linking: the loader runs a
//                 module's translation only when the module it places is the one translated (loader::GameModules)
// The module's SPU programs (ELF images in its segments) are translated into it too.

#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/prepare/prepare.hpp"

namespace pspocket::prepare {

// The lowest a game's module slot may start (D-238): 128 MB into the PRX region, well above the firmware's layout
// (about 46 MB with firmware 4.92), so a preparation needs no firmware to lay a game's modules out. The loader still
// refuses a library whose slots the firmware's layout reaches (loader::GameModules).
inline constexpr u32 kGameModuleFloor = 0x38000000;

struct ModulesOptions {
    std::vector<std::string> inputs; // folders of saved modules (<identity>.elf, <identity>.txt)
    std::string title_id;            // the game's
    std::string executable_sha256;   // the game's executable: a modules library is for it only
    u32 floor = kGameModuleFloor;    // slots stay above (and above the firmware's layout: LleModules::layout_end)
    std::string dir;                 // the modules library's folder (game_dir/modules)
    // As for a game (Options): the compiler, workers, memory, signing, QoS, progress, cancel, rates, sources kept.
    Toolchain toolchain;
    unsigned workers = 0;
    double memory_mb = 0;
    unsigned qos_class = 0;
    const codesign::SigningKey* key = nullptr;
    std::string identifier;
    double prior_rates[3] = {1.0, 1.0, 1.0};
    int thermal = 0;
    bool keep_sources = false;
    std::function<void(const Progress&)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

struct ModulesResult {
    Result library;          // prepare_game's (ok with no library when there are no modules: `none`)
    bool none = false;       // no module to translate: no library, dir/none written
    size_t modules = 0;      // translated
    size_t without_slot = 0; // no room left above the floor: they run from main memory, interpreted
    size_t unreadable = 0;   // saved files that are not modules this build can read
};

// Translates every module the inputs hold into dir. Slots of an earlier preparation for the same executable and floor
// are kept; new modules get theirs below.
ModulesResult prepare_game_modules(const ModulesOptions& options);

// Whether dir holds what the inputs need: a library (or `none`) made by this build for this executable, whose layout
// is above `floor` and holds every module the inputs hold. With no module saved and nothing prepared: true (*why
// "none").
bool game_modules_are_current(const std::string& dir, const std::string& executable_sha256,
    const std::vector<std::string>& inputs, u32 floor, std::string* why);

} // namespace pspocket::prepare
