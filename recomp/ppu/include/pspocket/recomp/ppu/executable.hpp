#pragma once

// A whole PPU executable translated (ppu-lift --elf, D-092/D-101), as a function: what the Mac's ppu-lift does and what
// the phone does when it prepares a game (route B, stage 4).

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/ppu/analyzer.hpp"
#include "pspocket/recomp/ppu/code_image.hpp"
#include "pspocket/recomp/ppu/lifter.hpp"
#include "pspocket/recomp/result.hpp"

namespace pspocket::recomp::ppu {

// An executable's loaded segments as a code image (bss as zeros), with the seeds it gives: its entry and its function
// descriptors (with their TOCs).
struct LoadedExecutable {
    CodeImage image;
    std::vector<Seed> seeds;
    std::optional<u32> toc;
    std::string summary; // segments, entry, function descriptors and TOC, as ppu-lift prints them
};
Result<LoadedExecutable> load_executable(std::span<const u8> elf);

struct ExecutableTranslation {
    Program program;   // the analysis: functions, and what was not supported (unsupported, illegal, warnings)
    LiftResult lifted; // the C translation units, the header and the registration unit
    std::string summary; // segments, entry, function descriptors and TOC, as ppu-lift prints them
};

// Every function of the executable (load_executable), analysed and lifted with `lift`.
Result<ExecutableTranslation> translate_executable(std::span<const u8> elf, const AnalyzerOptions& analyzer,
    const LiftOptions& lift);

} // namespace pspocket::recomp::ppu
