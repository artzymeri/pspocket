#pragma once

// A PRX module's segments placed at given addresses and relocated (RPCS3 rpcs3/Emu/Cell/PPUModule.cpp ppu_load_prx and
// its relocation types), as plain data. The firmware modules have one fixed layout (D-178), the same for every game:
// the loader copies a placed module into guest memory, and the system library's preparation translates the same
// bytes, so code translated once is valid wherever the module is loaded.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/result.hpp"

namespace pspocket::recomp {

struct PlacedSegment {
    u32 addr = 0; // 0: an empty PT_LOAD (it keeps its index)
    u32 memsz = 0;
    bool executable = false;
    // align_up(memsz, 0x100) bytes (relocations may write up to there, as into the 64 KB block a loader allocates) and
    // 8 more for the widest relocation there: the file's contents, then zeros.
    std::vector<u8> bytes;
};

struct PlacedPrx {
    std::vector<PlacedSegment> segments; // one per PT_LOAD, in order
    u32 relocations = 0;                 // applied
    std::vector<std::string> errors;     // relocations outside their segments or of a type RPCS3 lacks too (skipped)
};

// Places every PT_LOAD of `elf` at addrs[i] (one per PT_LOAD; 0 for an empty one) and applies its SCE relocations.
Result<PlacedPrx> place_prx(const ElfFile& elf, std::span<const u8> bytes, std::span<const u32> addrs);

// Each PT_LOAD's memory size (0 for an empty one), in order: what a layout needs to know of a module.
std::vector<u32> prx_load_sizes(const ElfFile& elf);

// The firmware's fixed layout (D-178): the modules' PT_LOADs one after another from `base`, in the order given, each at
// a 64 KB boundary (RPCS3 allocates each segment in 64 KB blocks); empty PT_LOADs get 0. Empty when they do not fit
// below base + size.
std::vector<std::vector<u32>> prx_fixed_layout(std::span<const std::vector<u32>> sizes, u32 base, u32 size);

} // namespace pspocket::recomp
