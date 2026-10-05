#pragma once

// A game's SPU programs lifted for its library (route B, stage 4c, D-155): what scripts/lift-spu.sh does on the Mac
// (D-076, D-081, D-099), in the pipeline, so the phone does it too.

#include <cstddef>
#include <string>
#include <vector>

namespace pspocket::prepare {

struct SpuLiftReport {
    size_t lifted = 0, failed = 0, merged = 0, seeds = 0;
};

// Lifts every SPU image a run saved in dump_dir (spu_<hash>_<ls>.bin with its .txt: pspocket-boot --spu-dump, or the
// app while the game runs) into out_dir: one directory per program and game_spu.c, whose pspocket_game_spu_register()
// registers them all. Seeds: the images' own "seed" lines, the runs' logs ("no lifted code at ... in program ..."),
// and every entry of the programs an earlier lift left in out_dir. Relocatable images (jobs, modules) are lifted once
// per hash, at the first copy's base. Programs out_dir held that are not lifted now are removed.
bool lift_spu_images(const std::string& dump_dir, const std::string& out_dir, const std::vector<std::string>& logs,
    SpuLiftReport* report, std::string* error);

} // namespace pspocket::prepare
