#pragma once

// Firmware modules run as they are (LLE, route B stage 5, D-157): a game's import library that pspocket has no host
// implementation of at all is served by the PS3's own module from /dev_flash (installed from the user's PS3UPDAT.PUP,
// D-156): decrypted, placed in main memory, relocated, its own imports linked (to pspocket's host modules, or to other
// firmware modules), its exports given to the game, and its module_start run on the main thread before the game's
// entry, as RPCS3 does (rpcs3/Emu/Cell/PPUModule.cpp: ppu_load_prx, ppu_load_exports, ppu_load_imports; the module
// list and its LLE/HLE choices: Emu/Cell/lv2/sys_prx.cpp g_prx_list). A module file goes LLE only when pspocket's host
// modules implement none of the libraries it exports, so the ones pspocket runs on the host (lv2, SPURS, the RSX,
// files, sysutil, network, NP) are never doubled. Firmware code runs under the interpreter.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle {
class HleProcess;
}

namespace pspocket::loader {

struct PrxModule {
    std::string name; // the module info's ("cellSync")
    std::string file; // its file in /dev_flash/sys/external ("libsync2.sprx"); empty for load_prx
    u32 toc = 0;
    std::vector<std::pair<u32, u32>> segments;             // where its segments went: (address, size)
    std::map<std::pair<std::string, u32>, u32> functions; // exported (library, NID) -> function descriptor
    std::map<std::pair<std::string, u32>, u32> variables; // exported (library, NID) -> variable address
    u32 start = 0, stop = 0;                              // module_start, module_stop descriptors (0: none)
    u32 relocations = 0;                                  // applied
    u32 relocation_errors = 0;                            // out of their segments, or of a type RPCS3 lacks too
};

class LleModules {
public:
    explicit LleModules(std::string dev_flash_dir);
    ~LleModules();

    // The libraries the firmware's modules export (sys/external/*.sprx), read once and kept beside /dev_flash
    // (lle-index.txt). False when there is no firmware or nothing can be read.
    bool index(std::string* error = nullptr);
    std::optional<std::string> file_for(std::string_view library) const;
    size_t libraries() const { return library_file_.size(); }

    // Whether a game's import library goes to its firmware module: the firmware has it and pspocket's host modules
    // implement none of the libraries that module exports. `forced` libraries (PSPOCKET_LLE, tests) go to their
    // firmware module whatever the host has.
    bool chooses(std::string_view library) const;
    void force(std::string library) { forced_.push_back(std::move(library)); }

    // Loads the module exporting `library` (once), with the modules its own imports need that the host lacks. Must be
    // called before guest threads run. nullptr with the reason in *error.
    const PrxModule* load(std::string_view library, hle::HleProcess& hle, std::string* error);
    const std::vector<std::unique_ptr<PrxModule>>& loaded() const { return loaded_; }
    // module_start descriptors in the order they run: a module after the modules it imports from.
    const std::vector<u32>& starts() const { return starts_; }

private:
    const PrxModule* load_file(const std::string& file, hle::HleProcess& hle, std::string* error);

    std::string dev_flash_;
    std::map<std::string, std::string, std::less<>> library_file_;               // library -> file
    std::map<std::string, std::vector<std::string>, std::less<>> file_libraries_; // file -> its libraries
    std::vector<std::string> forced_;
    std::vector<std::unique_ptr<PrxModule>> loaded_;
    std::vector<u32> starts_;
};

// Places a decrypted PRX (an ELF) in main memory and links it: segments, relocations (the SCE_PPURELA program
// header's entries, RPCS3's types 1, 4, 5, 6, 10, 11, 38, 44, 57), exports, and its imports through `link` (library,
// NID -> descriptor; 0 leaves the slot as it is, with an error logged).
bool load_prx(std::span<const u8> elf, hle::HleProcess& hle,
    const std::function<u32(std::string_view library, u32 nid)>& link, PrxModule* out, std::string* error);

} // namespace pspocket::loader
