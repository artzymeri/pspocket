#pragma once

// The system library's host side (D-178, pspocket/prepare/system.hpp): the firmware's modules translated once per
// firmware. Loaded before the executable's imports are linked; its lifted functions go to the PRX region's dispatch
// table (pspocket/ppu/prx_dispatch.hpp). Each firmware module that loads (LleModules' load hook) is checked against
// what was translated (modules.txt: its code segments' SHA-256 in guest memory); its translation runs only on a match,
// else the interpreter runs it, logged.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket {
class GuestMemory;
}

namespace pspocket::loader {

class LleModules;
struct PrxModule;

class SystemLibrary {
public:
    // Loads dir/game.dylib when it was made for `lle`'s modules (LleModules::identity_text) and this runtime, and
    // installs the load hook on `lle`. nullptr with the reason in *why otherwise (a missing library is not an error:
    // *why says so).
    static std::unique_ptr<SystemLibrary> load(const std::string& dir, LleModules& lle, const GuestMemory& memory,
        std::string* why);

    // A module just placed and linked: its translation runs if its code is the translated code.
    void check(const PrxModule& module);

    size_t modules_translated() const { return translated_; }
    size_t modules_refused() const { return refused_; }

private:
    struct Segment {
        u32 addr = 0, size = 0;
        std::string sha256;
    };
    explicit SystemLibrary(const GuestMemory& memory) : memory_(memory) {}
    const GuestMemory& memory_;
    std::map<std::string, std::vector<Segment>, std::less<>> modules_; // key -> code segments
    size_t translated_ = 0, refused_ = 0;
};

} // namespace pspocket::loader
