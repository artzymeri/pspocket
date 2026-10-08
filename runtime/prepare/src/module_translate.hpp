#pragma once

// A PRX module placed at fixed addresses and added to a translation (D-178; game modules, D-238): what the system
// library and a game's modules library share.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/ppu/code_image.hpp"
#include "pspocket/recomp/ppu/analyzer.hpp"
#include "pspocket/recomp/prx_place.hpp"

namespace pspocket::prepare::detail {

// Places `elf` at `addrs` and adds it to `image`: its segments; to `checks`, a line per code segment ("M <key> <addr>
// <size> <sha256>", what the loader compares the module with before its translation runs); to `seeds`, every exported
// function (module_start and the rest) and its descriptor table. The placed module, or none (logged) when it cannot
// be placed.
std::optional<recomp::PlacedPrx> add_placed_module(std::string_view key, const recomp::ElfFile& elf,
    std::span<const u8> bytes, std::span<const u32> addrs, recomp::ppu::CodeImage& image,
    std::vector<recomp::ppu::Seed>& seeds, std::string& checks);

} // namespace pspocket::prepare::detail
