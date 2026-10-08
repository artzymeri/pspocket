#pragma once

// A game's own PRX modules translated ahead of time: the modules library's host side (D-238,
// pspocket/prepare/modules.hpp). Loaded before the game runs; its functions go to the PRX region's dispatch table. A
// module the game loads whose identity has a slot is placed there (RuntimeModules), compared before linking with what
// was translated (modules.txt), and runs translated on a match with nothing written into its code by linking; else
// it runs under the interpreter, logged.

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/loader/lle.hpp"
#include "pspocket/recomp/prx_place.hpp"

namespace pspocket {
class GuestMemory;
}

namespace pspocket::loader {

class GameModules final : public GameModuleTranslations {
public:
    // Loads dir/game.dylib when it was made for the executable with this SHA-256 and this runtime and its slots sit
    // above the firmware's layout (lle: may be null), and installs it on `modules`. nullptr with the reason in *why
    // otherwise (no library, or `none`, is not an error: *why says so).
    static std::unique_ptr<GameModules> load(const std::string& dir, const std::string& executable_sha256,
        RuntimeModules& modules, const LleModules* lle, const GuestMemory& memory, std::string* why);
    ~GameModules() override;

    std::vector<u32> slot(std::string_view identity) const override;
    bool matches(std::string_view identity, const PrxModule& placed) const override;
    void enable(std::string_view identity, const PrxModule& module) override;
    bool enabled(std::string_view identity) const override;

    size_t modules() const { return code_.size(); }
    size_t modules_translated() const { return translated_.load(); }

private:
    struct Segment {
        u32 addr = 0, size = 0;
        std::string sha256;
    };
    GameModules(const GuestMemory& memory, RuntimeModules& modules) : memory_(memory), runtime_(modules) {}
    const GuestMemory& memory_;
    RuntimeModules& runtime_;
    recomp::GameModuleLayout layout_;
    std::map<std::string, std::vector<Segment>, std::less<>> code_; // identity -> code segments
    std::atomic<size_t> translated_{0};
    mutable std::mutex mutex_;
    std::set<std::string, std::less<>> enabled_; // identities whose translation runs
};

} // namespace pspocket::loader
