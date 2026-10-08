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

// A game module's identity (D-238): the SHA-256, in hex, of its ELF header, its program header table and each program
// header's file bytes, in order. Bytes after the module (an archive it was read from by offset) and its section table
// do not change it; any code or data byte does. The same module decrypted from a debug SELF has the same identity.
std::string prx_identity(const ElfFile& elf);

// Where a game's own modules go (D-238): top down from `below` (the top of the PRX region, or the lowest slot given so
// far), each module one block of its PT_LOADs at 64 KB boundaries (as prx_fixed_layout), never below `floor` (the end
// of the firmware's layout). Slots given before are never moved: a game's next preparation appends. A module that does
// not fit gets no addresses (it runs from main memory, interpreted) and the next one is still tried.
std::vector<std::vector<u32>> game_module_layout(std::span<const std::vector<u32>> sizes, u32 below, u32 floor);

// What a game's modules library was laid out for: the executable (its SHA-256), the floor, and each module's identity,
// slot (one address per PT_LOAD) and where it came from. As text, one line each: "E <sha256>", "F <floor>",
// "G <identity> <count> <address>... <source>".
struct GameModuleLayout {
    struct Module {
        std::string identity;
        std::vector<u32> addrs;
        std::string source;
    };
    std::string executable_sha256;
    u32 floor = 0;
    std::vector<Module> modules;
    // The lowest address any module's slot starts at (the top of the PRX region with none).
    u32 lowest() const;
    const Module* find(std::string_view identity) const;
};
std::string game_module_layout_text(const GameModuleLayout& layout);
Result<GameModuleLayout> parse_game_module_layout(std::string_view text);

} // namespace pspocket::recomp
