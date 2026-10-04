#pragma once

// The game's SPU programs, lifted (D-076): what SPURS attaches when it loads a task, a job or a policy module
// (runtime/hle/src/spu_programs.hpp), and where the images without a lifted program go for scripts/lift-spu.sh.

#include <string>

#include "pspocket/core/types.hpp"

namespace pspocket::hle {

// Saves each SPU image that has no lifted program into `dir` (once per image: the code, and a text file with its
// kind, guest address, LS address, size and entry). Empty: saves nothing. The images come from the user's game: keep
// them in the gitignored inventory/ (CLAUDE.md rule 1).
void set_spu_dump_dir(std::string dir);

struct SpuProgramStats {
    u64 attached = 0; // loads that found a lifted program
    u64 unknown = 0;  // loads that didn't (the interpreter ran them)
    u64 images = 0;   // distinct images without one, seen so far
};
SpuProgramStats spu_program_stats();

} // namespace pspocket::hle
