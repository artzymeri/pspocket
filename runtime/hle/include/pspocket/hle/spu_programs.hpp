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

// Code SPU programs load themselves (D-079): watches every SPU's GETs into local store and, when execution reaches an
// address no attached program has lifted code for, attaches the lifted program of the module copied there. Call once,
// before SPU threads run, when lifted programs are built in or images are being saved.
void enable_spu_modules();

struct SpuProgramStats {
    u64 attached = 0; // loads that found a lifted program
    u64 unknown = 0;  // loads that didn't (the interpreter ran them)
    u64 images = 0;   // distinct images without one, seen so far
    u64 modules = 0;  // modules (code a program loaded itself) that found a lifted program
    u64 modified = 0; // module loads changed in local store before they ran (interpreted: lifted code could be stale)
    u64 found_again = 0; // of the lifted loads, those known from an earlier load without hashing (D-109)
    u64 none_again = 0;  // of the interpreted loads, those known to have no lifted program without hashing (D-232)
    u64 reidentified = 0; // images loaded with no lifted program that found one registered later (D-234)
};
SpuProgramStats spu_program_stats();

// Job chain jobs whose descriptor changed between the visit that took them and their start (D-222): the job runs with
// what memory holds at its start.
u64 job_descriptors_changed();

} // namespace pspocket::hle
