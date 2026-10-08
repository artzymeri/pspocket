#pragma once

// Inside the loader: RPCS3's ppu_patch_refs (PPUModule.cpp), for the executable's imports (loader.cpp) and the firmware
// modules' (lle.cpp).

#include <string_view>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/types.hpp"

namespace pspocket::loader {

// A reference table of {type, addr, addend} entries ending with type 0: each place gets `value` (plus its addend) as
// its type says (ADDR32, ADDR16_LO, ADDR16_HA, ADDR16_LO_DS).
void patch_refs(GuestMemory& mem, u32 table, u32 value, std::string_view what);

// While one lives, the addresses patch_refs writes on this thread are added to `out` (D-238: whether linking a module
// wrote into its code).
class WatchRefs {
public:
    explicit WatchRefs(std::vector<u32>* out);
    ~WatchRefs();
    WatchRefs(const WatchRefs&) = delete;
    WatchRefs& operator=(const WatchRefs&) = delete;

private:
    std::vector<u32>* previous_;
};

} // namespace pspocket::loader
